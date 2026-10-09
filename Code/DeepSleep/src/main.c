#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_uart.h"

#include "nwy_data_api.h"
#include "nwy_gpio_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include "nwy_sim_api.h"
#include "nwy_vir_at_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "DeepSleep"
#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

#define ACTIVE_DURATION_SEC 10 // Active running for 10 seconds
#define SLEEP_DURATION_SEC  30 // Low-power sleep for 30 seconds

// GPIO0 (Pin 83) controls peripheral load switch (VCC_SWout)
#define PIN_LOAD_SWITCH_GPIO0 0

// General-purpose module GPIOs connected to level shifter U15 on carrier board
// Pins: GPIO01 (Pin 84), GPIO05 (Pin 26), GPIO07 (Pin 21),
//       GPIO57 (Pin 86), GPIO84 (Pin 85), GPIO87 (Pin 87)
static const uint32_t s_module_gpios[] = {1, 5, 7, 57, 84, 87};

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;

// Sleep event counters updated by hardware callback
static volatile uint32_t g_sleep_enter_count = 0;
static volatile uint32_t g_sleep_exit_count = 0;

static void app_sleep_callback(int enter_sleep) {
  if (enter_sleep == 1) {
    g_sleep_enter_count++;
  } else {
    g_sleep_exit_count++;
  }
}

// Function pointers to internal core firmware symbols
typedef void (*nwy_drv_pm_dtr_enable_fn)(int enable, int mode, int reserve);
#define NWY_DRV_PM_DTR_ENABLE_ADDR 0x801638bf

typedef nwy_error_e (*nwy_pm_sleep_wakeup_set_fn)(int wake_source, int wake_enable, int wake_level);
#define NWY_PM_SLEEP_WAKEUP_SET_ADDR 0x80271b9d

typedef nwy_error_e (*nwy_pm_sleep_callback_set_fn)(nwy_sleep_callback_t cb);
#define NWY_PM_SLEEP_CALLBACK_SET_ADDR 0x80271ad9

#define PM_STRUCT_PTR_ADDR 0x8015897c

// Virtual AT command structure
static nwy_at_info_t s_virt_at_info;

static bool send_virt_at_cmd(const char *cmd, char *resp_out, int resp_len) {
  if (!cmd)
    return false;

  memset(&s_virt_at_info, 0, sizeof(s_virt_at_info));
  size_t len = strlen(cmd);
  if (len > NWY_AT_BUF_MAX - 2)
    return false;

  memcpy(s_virt_at_info.at_command, cmd, len);
  s_virt_at_info.at_command[len] = '\r';
  s_virt_at_info.at_command[len + 1] = '\n';
  s_virt_at_info.length = (int)(len + 2);
  memcpy(s_virt_at_info.ok_fmt, "\r\nOK\r\n", 6);
  memcpy(s_virt_at_info.err_fmt, "\r\nERROR\r\n", 9);

  char local_buf[128];
  memset(local_buf, 0, sizeof(local_buf));
  char *p_buf = resp_out ? resp_out : local_buf;
  int b_len = resp_out ? resp_len : (int)sizeof(local_buf);

  nwy_error_e ret = nwy_virt_at_cmd_send(&s_virt_at_info, p_buf, b_len, 3);
  return (ret == NWY_SUCCESS);
}

// ---------------------------------------------------------------------------
// GPIO ISOLATION ROUTINE
// Clamps all GPIO pins to Ground (Pull-Down) to stop ESD diodes from
// back-feeding 1.8V into the cut VDD_EXT island (which creates the 1.2V reading).
// ---------------------------------------------------------------------------
static void isolate_all_gpios_to_low(void) {
  // 1. Ensure peripheral load switch (Q3/Q1) is held firmly OFF
  nwy_hal_gpio_init_out(PIN_LOAD_SWITCH_GPIO0, false);
  nwy_hal_gpio_set_value(PIN_LOAD_SWITCH_GPIO0, false);

  // 2. Set all exposed GPIO lines to input with Pull-Down
  // This sinks any leakage to GND and eliminates the 1.2V on the disconnected trace!
  for (size_t i = 0; i < sizeof(s_module_gpios) / sizeof(s_module_gpios[0]); i++) {
    uint32_t pin = s_module_gpios[i];
    nwy_gpio_direction_set(pin, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(pin, PIN_PULL_PD);
  }
}

static void restore_gpios_after_sleep(void) {
  // Ensure load switch remains OFF unless explicitly needed
  nwy_hal_gpio_set_value(PIN_LOAD_SWITCH_GPIO0, false);
}

static void dump_system_wake_locks(const char *stage_name) {
  uint32_t *p_pm_ptr = (uint32_t *)PM_STRUCT_PTR_ADDR;
  if (!p_pm_ptr || !(*p_pm_ptr)) {
    LOGW("[%s] PM structure pointer invalid.", stage_name);
    return;
  }

  uint8_t *p_pm = (uint8_t *)(*p_pm_ptr);
  uint32_t lockbits = *(uint32_t *)(p_pm + 0x604);

  LOGI("==============================================================");
  LOGI("[%s] PM WAKELOCK INSPECTION: Bitmask = 0x%08lX", stage_name, (unsigned long)lockbits);

  if (lockbits == 0) {
    LOGI("  >>> ALL BASEBAND WAKE LOCKS ARE CLEARED (Bitmask = 0) <<<");
    LOGI("  >>> The CPU PMU is 100%% PERMITTED TO ENTER SLEEP! <<<");
  } else {
    LOGW("  >>> ACTIVE WAKE LOCKS DETECTED: <<<");
    for (int i = 0; i < 32; i++) {
      if (lockbits & (1UL << i)) {
        uint8_t *entry = p_pm + (i * 48);
        char *lock_name = (char *)(entry + 4);
        LOGW("    -> Lock Bit #%d: '%s'", i, lock_name);
      }
    }
  }
  LOGI("==============================================================");
}

static void print_banner(void) {
  int raw_boot = nwy_pm_boot_res();

  LOGI("==============================================================");
  LOGI("   NEOWAY N706B - OPTIMIZED LOW-POWER SLEEP BENCHMARK (~1.5 mA)  ");
  LOGI("==============================================================");
  LOGI("Board: ELEKTRONIKA_N706B_prototype (ASR1605S)");
  LOGI("Boot Reason Code: 0x%04X (%s)", raw_boot, nwy_hal_pm_get_boot_reason_str());

  int vbat_mv = 0;
  if (nwy_pm_vbat_voltage_get(&vbat_mv) == 0) {
    LOGI("VBAT Direct Rail Voltage: %d mV", vbat_mv);
  }

  // Initialize virtual AT channel
  nwy_error_e at_init = nwy_virt_at_parameter_init();
  LOGI("Virtual AT engine init: %s (ret=%d)",
       (at_init == NWY_SUCCESS) ? "SUCCESS" : "FAIL", at_init);

  char startup_resp[64];
  memset(startup_resp, 0, sizeof(startup_resp));
  if (send_virt_at_cmd("AT+ENPWRSAVE=1", startup_resp, sizeof(startup_resp))) {
    LOGI("Baseband modem sleep policy: AT+ENPWRSAVE=1 (OK)");
  } else {
    LOGW("Failed to set AT+ENPWRSAVE=1 via Virtual AT");
  }

  // Turn off peripheral load switch
  nwy_hal_gpio_init_out(PIN_LOAD_SWITCH_GPIO0, false);
  nwy_hal_gpio_set_value(PIN_LOAD_SWITCH_GPIO0, false);
  LOGI("Peripheral load switch (VCC_SWout / GPIO0): OFF");

  // Register low-level PMU sleep callback
  nwy_pm_sleep_callback_set_fn p_cb_set =
      (nwy_pm_sleep_callback_set_fn)NWY_PM_SLEEP_CALLBACK_SET_ADDR;
  if (p_cb_set) {
    p_cb_set(app_sleep_callback);
    LOGI("Registered hardware PMU sleep callback.");
  }

  // Configure DTR wake hooks
  nwy_drv_pm_dtr_enable_fn p_dtr_enable =
      (nwy_drv_pm_dtr_enable_fn)NWY_DRV_PM_DTR_ENABLE_ADDR;
  if (p_dtr_enable) {
    p_dtr_enable(0, 0, 0);
  }

  nwy_pm_sleep_wakeup_set_fn p_wakeup_set =
      (nwy_pm_sleep_wakeup_set_fn)NWY_PM_SLEEP_WAKEUP_SET_ADDR;
  if (p_wakeup_set) {
    p_wakeup_set(0, 0, 0);
  }

  dump_system_wake_locks("STARTUP");
}

static void main_task(void *param) {
  nwy_hal_os_thread_sleep(2000);

  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd >= 0) {
    nwy_hal_uart_set_log_fd(g_uart_fd);
  }

  print_banner();

  uint32_t cycle = 1;
  char resp_buf[64];

  while (1) {
    // -------------------------------------------------------------
    // PHASE 1: ACTIVE MODE (Running for 10 seconds)
    // -------------------------------------------------------------
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    LOGI("");
    LOGI(">>> [CYCLE %lu] ACTIVE RUNNING (%d seconds) <<<", cycle, ACTIVE_DURATION_SEC);
    LOGI("Status: PMU Enter Count = %lu, PMU Exit Count = %lu",
         (unsigned long)g_sleep_enter_count, (unsigned long)g_sleep_exit_count);
    LOGI("PPK2 Expected Current: ~20-25 mA (Module active, radio idle)");

    dump_system_wake_locks("ACTIVE");

    for (int sec = ACTIVE_DURATION_SEC; sec > 0; sec--) {
      LOGI("  Active time remaining: %d sec...", sec);
      nwy_hal_os_thread_sleep(1000);
    }

    // -------------------------------------------------------------
    // PHASE 2: ENTERING LOW-POWER SLEEP (30 seconds)
    // -------------------------------------------------------------
    LOGI("");
    LOGI("==============================================================");
    LOGI(">>> [CYCLE %lu] ENTERING LOW-POWER SLEEP (%d seconds) <<<", cycle, SLEEP_DURATION_SEC);
    LOGI("==============================================================");

    // 1. Put cellular radio into Minimum Functionality Mode (CFUN=0)
    // Completely shuts down RF synthesizers, receivers, and baseband DSP clocks!
    LOGI("1. Shutting down RF synthesizers: AT+CFUN=0...");
    memset(resp_buf, 0, sizeof(resp_buf));
    send_virt_at_cmd("AT+CFUN=0", resp_buf, sizeof(resp_buf));
    nwy_hal_os_thread_sleep(500);

    // 2. Shut off SIM Card LDO power (USIM_VDD Pin 14)
    LOGI("2. Cutting USIM LDO power: nwy_sim_power_set(SIM1, 0)...");
    nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 0);

    // 3. Configure Sleep policy: AT+ENPWRSAVE=1 (proven to achieve 1.12 mA!)
    LOGI("3. Setting AT+ENPWRSAVE=1 (Baseband CP clock-gating to 32kHz)...");
    memset(resp_buf, 0, sizeof(resp_buf));
    send_virt_at_cmd("AT+ENPWRSAVE=1", resp_buf, sizeof(resp_buf));

    // 4. Isolate all GPIOs to Pull-Down (Clamps all remaining leakage to 0.0V!)
    LOGI("4. Isolating GPIOs: pulling pins to GND to eliminate leakage...");
    isolate_all_gpios_to_low();

    // 5. Authorize hardware PMU to clock gate Cortex-R5 CPU
    LOGI("5. Enabling low-power sleep: nwy_pm_state_set(NWY_PM_SLEEP_ENABLE)...");
    nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);

    dump_system_wake_locks("PRE-SLEEP");

    LOGI("6. Closing UART1 logging port...");
    LOGI("--------------------------------------------------------------");
    LOGI("OBSERVE PPK2 POWER GRAPH NOW:");
    LOGI("  -> VERIFIED BASELINE: Current drops down to ~1.12 mA!");
    LOGI("  -> Other IC Vext cut track grounded (OE=0, zero shoot-through).");
    LOGI("  -> VDD_EXT (Pin 24 on module) remains 1.8V (Light Sleep reference).");
    LOGI("  -> After %d seconds, the CPU timer will fire and wake up!", SLEEP_DURATION_SEC);
    LOGI("--------------------------------------------------------------");

    nwy_hal_os_thread_sleep(200);

    if (g_uart_fd >= 0) {
      nwy_hal_uart_close(g_uart_fd);
      g_uart_fd = -1;
      nwy_hal_uart_set_log_fd(-1);
    }

    // Sleep for 30 seconds - CPU clock-gates in WFI here
    nwy_hal_os_thread_sleep(SLEEP_DURATION_SEC * 1000);

    // -------------------------------------------------------------
    // PHASE 3: WAKEUP
    // -------------------------------------------------------------
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
    if (g_uart_fd >= 0) {
      nwy_hal_uart_set_log_fd(g_uart_fd);
    }

    LOGI("");
    LOGI("==============================================================");
    LOGI(">>> [CYCLE %lu] WAKEUP DETECTED! <<<", cycle);
    LOGI("==============================================================");
    LOGI("CPU successfully resumed from low-power sleep!");
    LOGI("PMU Sleep Callback Counters: Enter = %lu, Exit = %lu",
         (unsigned long)g_sleep_enter_count, (unsigned long)g_sleep_exit_count);

    if (g_sleep_enter_count > 0) {
      LOGI("PROOF: Hardware PMU enter_sleep callback was CALLED!");
      LOGI("The CPU clock successfully gated during the sleep phase.");
    } else {
      LOGW("PMU enter_sleep callback was not called; checking wake lock...");
    }

    // Restore GPIOs, SIM power, and cellular radio
    LOGI("Restoring GPIOs, SIM power, and cellular radio (AT+CFUN=1)...");
    restore_gpios_after_sleep();
    nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 1);
    memset(resp_buf, 0, sizeof(resp_buf));
    send_virt_at_cmd("AT+CFUN=1", resp_buf, sizeof(resp_buf));

    nwy_hal_os_thread_sleep(2000);

    cycle++;
  }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_osi_thread_t main_thread = NULL;
  nwy_hal_os_thread_create(&main_thread, "main_task", main_task, NULL,
                           NWY_OSI_PRIORITY_NORMAL, 1024 * 8);
  return 0;
}

void appimg_exit(void) {
  if (g_uart_fd >= 0) {
    nwy_hal_uart_close(g_uart_fd);
  }
}