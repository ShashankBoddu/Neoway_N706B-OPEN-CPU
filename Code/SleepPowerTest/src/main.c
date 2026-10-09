#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_uart.h"

#include "nwy_data_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include "nwy_vir_at_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SleepPowerTest"
#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

// On ELEKTRONIKA_N706B_prototype:
// GPIO0 (Pin 83) controls Q3/Q1 load switch for VCC_SWout.
// Setting it LOW (0) turns OFF VCC_SWout to save power.
#define PIN_LOAD_SWITCH_GPIO0 0

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;

// Statically allocated to prevent stack overflow in RTOS task
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

  char local_buf[256];
  memset(local_buf, 0, sizeof(local_buf));
  char *p_buf = resp_out ? resp_out : local_buf;
  int b_len = resp_out ? resp_len : (int)sizeof(local_buf);

  nwy_error_e ret = nwy_virt_at_cmd_send(&s_virt_at_info, p_buf, b_len, 3);
  return (ret == NWY_SUCCESS);
}

static void configure_sleep_policy(void) {
  char resp[128];

  LOGI("--------------------------------------------------------------");
  LOGI("Configuring Modem Sleep Policy via Virtual AT...");

  // Initialize virtual AT channel inside OpenCPU
  nwy_error_e init_ret = nwy_virt_at_parameter_init();
  LOGI("Virtual AT engine init: %s (ret=%d)",
       (init_ret == NWY_SUCCESS) ? "SUCCESS" : "FAIL", init_ret);

  // 1. Query initial sleep state
  memset(resp, 0, sizeof(resp));
  if (send_virt_at_cmd("AT+ENPWRSAVE?", resp, sizeof(resp))) {
    LOGI("Current AT+ENPWRSAVE: %s", resp);
  }

  // 2. Configure AT+ENPWRSAVE=2:
  //    Parameter 2: High level at DTR triggers sleep mode; Low level wakes up!
  //    On ELEKTRONIKA_N706B schematic:
  //    MAIN_DTR (Pin 19) is pulled UP to VDD_EXT (1.8V) through R3 (4.7 kOhm).
  //    Default AT+ENPWRSAVE=1 requires LOW level to sleep, which locked CPU awake.
  //    Setting mode 2 satisfies the sleep condition naturally!
  memset(resp, 0, sizeof(resp));
  if (send_virt_at_cmd("AT+ENPWRSAVE=2", resp, sizeof(resp))) {
    LOGI("Set AT+ENPWRSAVE=2 [DTR HIGH = SLEEP]: OK");
  } else {
    LOGW("Failed to set AT+ENPWRSAVE=2, trying AT+NWENPWRSAVE=2...");
    memset(resp, 0, sizeof(resp));
    send_virt_at_cmd("AT+NWENPWRSAVE=2", resp, sizeof(resp));
    LOGI("Set AT+NWENPWRSAVE=2: %s", resp);
  }

  // 3. Verify updated setting
  memset(resp, 0, sizeof(resp));
  if (send_virt_at_cmd("AT+ENPWRSAVE?", resp, sizeof(resp))) {
    LOGI("Verified AT+ENPWRSAVE query: %s", resp);
  }
  LOGI("--------------------------------------------------------------");
}

static void print_banner(void) {
  LOGI("==============================================================");
  LOGI("   NEOWAY N706B OPENCPU - SLEEP MODE CURRENT BENCHMARK        ");
  LOGI("==============================================================");
  LOGI("Board: ELEKTRONIKA_N706B_prototype (ASR1605S)");
  LOGI("Boot Reason: %s (Raw: 0x%02X)", nwy_hal_pm_get_boot_reason_str(),
       nwy_pm_boot_res());

  int vbat_mv = 0;
  if (nwy_pm_vbat_voltage_get(&vbat_mv) == 0) {
    LOGI("VBAT Voltage: %d mV", vbat_mv);
  }

  // Ensure external peripheral load switch is OFF
  nwy_hal_gpio_init_out(PIN_LOAD_SWITCH_GPIO0, false);
  nwy_hal_gpio_set_value(PIN_LOAD_SWITCH_GPIO0, false);
  LOGI("Peripheral load switch (VCC_SWout / GPIO0) set to OFF.");

  configure_sleep_policy();

  LOGI("CRITICAL PPK2 NOTE: Disconnect USB cable from PC!");
  LOGI("USB VBUS keeps USB PHY awake (+15 mA) and blocks CPU sleep.");
  LOGI("Power: PPK2 5V on Vin & GND. Logs: Hardware UART1 only.");
  LOGI("==============================================================");
}

static void main_task(void *param) {
  // 1. Initial settling delay for power and UART
  nwy_hal_os_thread_sleep(2000);

  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd >= 0) {
    nwy_hal_uart_set_log_fd(g_uart_fd);
  }

  print_banner();

  uint32_t cycle = 1;

  while (1) {
    // =============================================================
    // PHASE 1: ACTIVE MODE (Running for 10 seconds)
    // =============================================================
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    LOGI("");
    LOGI(">>> [CYCLE %lu] PHASE 1: ACTIVE MODE (10s) <<<", cycle);
    LOGI("CPU running at full frequency. Radio active.");
    LOGI("PPK2 Expected Current: ~35 mA - 50 mA @ Vin=5V");

    for (int sec = 10; sec > 0; sec--) {
      LOGI("  Active remaining: %d sec...", sec);
      nwy_hal_os_thread_sleep(1000);
    }

    // =============================================================
    // PHASE 2: MEDIUM SLEEP MODE - LTE CONNECTED (15 seconds)
    // =============================================================
    LOGI("");
    LOGI(">>> [CYCLE %lu] PHASE 2: MEDIUM SLEEP - LTE CONNECTED (15s) <<<", cycle);
    LOGI("LTE network connected (DRX paging enabled).");
    LOGI("PPK2 Expected Current: Drops to ~2 mA - 5 mA average");
    LOGI("(Small periodic spikes to ~40-80 mA are normal LTE paging bursts)");
    LOGI("Closing UART logging port & gating CPU clock now...");

    // Allow UART transmit FIFO to flush
    nwy_hal_os_thread_sleep(200);

    // Close UART so its clock gating and low-power modes activate
    if (g_uart_fd >= 0) {
      nwy_hal_uart_close(g_uart_fd);
      g_uart_fd = -1;
      nwy_hal_uart_set_log_fd(-1);
    }

    // Enable OpenCPU sleep: PMU gates Cortex-R5 clock in FreeRTOS idle task
    nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);

    // Sleep for 15 seconds: OS timer interrupts CPU to wake it up
    nwy_hal_os_thread_sleep(15000);

    // Wakeup: Lock CPU awake and reopen UART
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);
    g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
    if (g_uart_fd >= 0) {
      nwy_hal_uart_set_log_fd(g_uart_fd);
    }

    LOGI("");
    LOGI(">>> [CYCLE %lu] PHASE 2 WAKEUP DETECTED! <<<", cycle);
    LOGI("Resumed successfully! Observe PPK2: current jumped back to active.");

    nwy_hal_os_thread_sleep(3000);

    // =============================================================
    // PHASE 3: RADIO-OFF SLEEP MODE - FLIGHT MODE (15 seconds)
    // =============================================================
    LOGI("");
    LOGI(">>> [CYCLE %lu] PHASE 3: RADIO-OFF SLEEP - FLIGHT MODE (15s) <<<", cycle);
    LOGI("Disabling cellular RF (Flight Mode) to eliminate paging spikes...");

    nwy_nw_radio_set(NWY_NW_RADIO_FLIGHT_MODE);
    nwy_hal_os_thread_sleep(1000);

    LOGI("PPK2 Expected Current: Flat ~1.5 mA - 2.5 mA line (NO spikes!)");
    LOGI("Closing UART logging port & gating CPU clock now...");

    nwy_hal_os_thread_sleep(200);

    if (g_uart_fd >= 0) {
      nwy_hal_uart_close(g_uart_fd);
      g_uart_fd = -1;
      nwy_hal_uart_set_log_fd(-1);
    }

    nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);

    nwy_hal_os_thread_sleep(15000);

    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);
    g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
    if (g_uart_fd >= 0) {
      nwy_hal_uart_set_log_fd(g_uart_fd);
    }

    LOGI("");
    LOGI(">>> [CYCLE %lu] PHASE 3 WAKEUP DETECTED! <<<", cycle);
    LOGI("Resumed successfully from Flight Mode sleep!");
    LOGI("Restoring cellular radio to NORMAL MODE...");

    nwy_nw_radio_set(NWY_NW_RADIO_NORMAL_MODE);

    nwy_hal_os_thread_sleep(3000);

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