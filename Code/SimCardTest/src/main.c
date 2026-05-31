#include "hal/os/nwy_hal_os.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/uart/nwy_hal_uart.h"
#include "nwy_log_api.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

#define TAG "SimTest"
#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;
// Redefine printf to map directly to the opened UART port
#define printf(fmt, ...) nwy_hal_uart_printf(g_uart_fd, fmt, ##__VA_ARGS__)

static const char *get_sim_status_str(nwy_sim_status_e status) {
  switch (status) {
  case NWY_SIM_NOT_INSERTED:
    return "NOT INSERTED";
  case NWY_SIM_READY:
    return "READY";
  case NWY_SIM_PIN_REQ:
    return "PIN REQUIRED";
  case NWY_SIM_PUK_REQ:
    return "PUK REQUIRED";
  case NWY_SIM_BUSY:
    return "BUSY";
  case NWY_SIM_BLOCKED:
    return "BLOCKED";
  case NWY_SIM_UNKNOWN:
  default:
    return "UNKNOWN";
  }
}

static const char *get_pin_mode_str(nwy_sim_pin_mode_e mode) {
  switch (mode) {
  case NWY_SIM_PIN_MODE_DISABLED:
    return "DISABLED";
  case NWY_SIM_PIN_MODE_ENABLED:
    return "ENABLED";
  case NWY_SIM_PIN_MODE_UNKOWN:
  default:
    return "UNKNOWN";
  }
}

// URC Callback for SIM Events
static void test_sim_urc_cb(int sim_id, bool card_present,
                            nwy_sim_status_e sim_status) {
  // Use tagged log info to notify SIM event
  LOGI("SIM %d Event: Present=%s, Status=%s (%d)", sim_id,
       card_present ? "YES" : "NO", get_sim_status_str(sim_status),
       (int)sim_status);
}

// Automated Non-Interactive SIM HAL Diagnostic Scan
static void run_sim_autodiagnostic(void) {
  printf("\r\n");
  printf("==================================================\r\n");
  printf("          SIM HAL DIAGNOSTIC SELF-TEST REPORT     \r\n");
  printf("==================================================\r\n");

  // 1. Query active slot
  uint8_t slot_id = 0xFF;
  bool slot_ok = nwy_hal_sim_get_active_slot(&slot_id);
  if (slot_ok) {
    LOGI("Active Slot Configured: %d (%s)", slot_id + 1,
         slot_id == 0 ? "Slot 1" : (slot_id == 1 ? "Slot 2" : "Unknown"));
  } else {
    LOGE("Active Slot Configured: FAILED TO QUERY");
  }

  // 2. Scan both slots
  for (int sim = 1; sim <= 2; sim++) {
    printf("--------------------------------------------------\r\n");
    LOGI("Scanning SIM Slot %d...", sim);

    // Check basic status
    nwy_sim_status_e raw_status = NWY_SIM_UNKNOWN;
    nwy_sim_status_get((sim == 2) ? NWY_SIM_ID_SLOT_2 : NWY_SIM_ID_SLOT_1,
                       &raw_status);
    bool ready = nwy_hal_sim_is_ready(sim);

    LOGI("SIM %d Presence/Status: %s (%d - %s)", sim,
         ready ? "READY/INSERTED" : "NOT READY", (int)raw_status,
         get_sim_status_str(raw_status));

    if (raw_status != NWY_SIM_NOT_INSERTED && raw_status != NWY_SIM_UNKNOWN) {
      // Get ICCID
      char iccid[32] = "N/A";
      if (nwy_hal_sim_get_iccid(sim, iccid)) {
        LOGI("  ICCID:  %s", iccid);
      } else {
        LOGE("  ICCID:  Failed to retrieve");
      }

      // Get IMSI
      char imsi[32] = "N/A";
      if (nwy_hal_sim_get_imsi(sim, imsi)) {
        LOGI("  IMSI:   %s", imsi);
      } else {
        LOGE("  IMSI:   Failed to retrieve");
      }

      // Get MSISDN
      char msisdn[32] = "N/A";
      if (nwy_hal_sim_get_msisdn(sim, msisdn, sizeof(msisdn))) {
        LOGI("  MSISDN: %s", msisdn);
      } else {
        LOGW("  MSISDN: Not available (Not stored on card)");
      }

      // Get PIN mode
      nwy_sim_pin_mode_e pin_mode = NWY_SIM_PIN_MODE_UNKOWN;
      if (nwy_hal_sim_get_pin_mode(sim, &pin_mode)) {
        LOGI("  PIN Mode: %s (%d)", get_pin_mode_str(pin_mode), (int)pin_mode);
      } else {
        LOGE("  PIN Mode: Failed to query");
      }

      // Get retry times
      uint8_t pin_retries = 0, puk_retries = 0;
      if (nwy_hal_sim_get_retry_times(sim, &pin_retries, &puk_retries)) {
        LOGI("  Retries Remaining: PIN=%d, PUK=%d", pin_retries, puk_retries);
      } else {
        LOGE("  Retries Remaining: Failed to query");
      }
    } else {
      LOGW("SIM Slot %d is empty or inactive.", sim);
    }
  }

  // 3. Test Hotplug URC interface
  printf("--------------------------------------------------\r\n");
  LOGI("Testing SIM Hotplug URC callback interface...");
  bool urc_reg1 = nwy_hal_sim_register_urc_cb(1, test_sim_urc_cb);
  bool urc_reg2 = nwy_hal_sim_register_urc_cb(2, test_sim_urc_cb);

  if (urc_reg1 && urc_reg2) {
    LOGI("URC Callback Register Test: PASSED");
  } else {
    LOGE("URC Callback Register Test: FAILED (Slot1=%d, Slot2=%d)", urc_reg1,
         urc_reg2);
  }

  printf("==================================================\r\n");
}

// Main background thread
static void main_monitor_task(void *param) {
  // Allow time for startup logs and stable power initialization
  nwy_hal_os_thread_sleep(3000);

  NWY_SDK_LOG_DEBUG("SimCardTest: Initializing test UART interface...");
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd < 0) {
    NWY_SDK_LOG_DEBUG("SimCardTest: Failed to open UART %s, error %d",
                      TEST_UART_PORT, g_uart_fd);
    return;
  }

  NWY_SDK_LOG_DEBUG("SimCardTest: UART %s opened (fd=%d)", TEST_UART_PORT,
                    g_uart_fd);

  // Explicitly configure default logging UART descriptor
  nwy_hal_uart_set_log_fd(g_uart_fd);

  // Print Welcome Menu
  printf("\r\n==================================================\r\n");
  printf("   NEOWAY N706B OPENCPU - SIM CARD API TEST V1      \r\n");
  printf("==================================================\r\n");
  printf("Port: UART4 (Debug) | Baudrate: 115200\r\n");

  // Register default URC on boot to catch insertion events immediately
  nwy_hal_sim_register_urc_cb(1, test_sim_urc_cb);
  nwy_hal_sim_register_urc_cb(2, test_sim_urc_cb);

  // Enable SIM hotplug hardware detection for both slots
  // (1 = Enable, 0 = Active Low trigger level - standard for most SIM card
  // slots)
  bool det_ok1 = nwy_hal_sim_set_detect(1, 1, 0);
  bool det_ok2 = nwy_hal_sim_set_detect(2, 1, 0);
  LOGI("SIM Hotplug Hardware Detection Configured: Slot1=%s, Slot2=%s",
       det_ok1 ? "ENABLED" : "FAILED", det_ok2 ? "ENABLED" : "FAILED");

  // Run the automated SIM diagnostics self-test on startup
  run_sim_autodiagnostic();

  while (1) {
    nwy_hal_os_thread_sleep(10000);
    LOGI("Heartbeat - background task alive");
  }
}

// Entry Point
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_thread_sleep(1000);
  NWY_SDK_LOG_DEBUG("SimCardTest: Application Entry");

  nwy_osi_thread_t monitor_thread = NULL;
  bool ok = nwy_hal_os_thread_create(&monitor_thread, "sim_monitor",
                                     main_monitor_task, NULL,
                                     NWY_OSI_PRIORITY_NORMAL, 1024 * 4);
  NWY_SDK_LOG_DEBUG("SimCardTest: Monitoring thread spawned, status = %d", ok);

  return 0;
}

// Exit Point
void appimg_exit(void) {
  NWY_SDK_LOG_DEBUG("SimCardTest: Application Exit");

  if (g_uart_fd >= 0) {
    nwy_hal_sim_unregister_urc_cb(1);
    nwy_hal_sim_unregister_urc_cb(2);
    nwy_hal_uart_close(g_uart_fd);
  }
}
