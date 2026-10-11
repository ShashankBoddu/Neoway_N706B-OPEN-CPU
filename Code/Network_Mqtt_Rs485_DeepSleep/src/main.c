#include "hal/nwy_hal_adc.h"
#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_mqtt.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_rs485.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_uart.h"

#include "nwy_data_api.h"
#include "nwy_gpio_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include "nwy_sim_api.h"
#include "nwy_uart_api.h"
#include "nwy_vir_at_api.h"
#include "nwy_dev_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "N706B_PROD"

#define DEBUG_UART_PORT "URT1"
#define DEBUG_UART_BAUD 115200

#define RS485_TEST_BAUD 9600
#define RS485_RX_TIMEOUT_MS 10000
#define SLEEP_DURATION_SEC 30

#define MQTT_BROKER_HOST "mqtt.fervidlabs.in"
#define MQTT_BROKER_PORT 1883
#define MQTT_USER "fervid"
#define MQTT_PASS "Fervid@123"
#define MQTT_PUB_TOPIC "nwy_n706b/fervid/tx"

#define PIN_PERIPHERAL_POWER_GPIO0 0

// Exposed GPIO lines to isolate during sleep
static const uint32_t s_module_gpios[] = {1, 5, 7, 57, 84, 87};

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_dbg_uart_fd = -1;
#define printf(fmt, ...) nwy_hal_uart_printf(g_dbg_uart_fd, fmt, ##__VA_ARGS__)

// Virtual AT command engine
static bool send_virt_at_cmd(const char *cmd, char *resp_out, int resp_len) {
  if (!cmd)
    return false;

  nwy_at_info_t info;
  memset(&info, 0, sizeof(info));
  size_t len = strlen(cmd);
  if (len > NWY_AT_BUF_MAX - 2)
    return false;

  memcpy(info.at_command, cmd, len);
  info.at_command[len] = '\r';
  info.at_command[len + 1] = '\n';
  info.length = (int)(len + 2);
  memcpy(info.ok_fmt, "OK", 3);
  memcpy(info.err_fmt, "ERROR", 6);

  char local_buf[128];
  memset(local_buf, 0, sizeof(local_buf));
  char *p_buf = resp_out ? resp_out : local_buf;
  int b_len = resp_out ? resp_len : (int)sizeof(local_buf);

  nwy_error_e ret = nwy_virt_at_cmd_send(&info, p_buf, b_len, 2);
  return (ret == NWY_SUCCESS);
}

// Data call state tracking
static volatile bool g_data_call_active = false;
static char g_local_ip[32] = {0};

static void data_call_callback(int sim_id, int profile_idx, bool connected,
                               const char *ipv4_addr) {
  g_data_call_active = connected;
  if (connected && ipv4_addr) {
    strncpy(g_local_ip, ipv4_addr, sizeof(g_local_ip) - 1);
    g_local_ip[sizeof(g_local_ip) - 1] = '\0';
    LOGI("Cellular Data Call established: IP=%s (CID=%d)", g_local_ip, profile_idx);
  } else {
    LOGW("Cellular Data Call disconnected (CID=%d)", profile_idx);
    memset(g_local_ip, 0, sizeof(g_local_ip));
  }
}

static void on_mqtt_message_rx(const char *topic, const uint8_t *payload,
                               uint32_t len) {
  LOGI("MQTT Inbound RX [%s]: %.*s", topic, (int)len, (const char *)payload);
}

static void isolate_gpios_for_sleep(void) {
  // 1. Cut Peripheral Master Power (GPIO0 = LOW)
  // Cuts Buck output to peripheral VBAT & THVD1420DR VCC (0.32V)
  // Cuts Vin from external sensor rail VCC_SWout (0.00V)
  nwy_hal_gpio_init_out(PIN_PERIPHERAL_POWER_GPIO0, false);
  nwy_hal_gpio_set_value(PIN_PERIPHERAL_POWER_GPIO0, false);

  // 2. Set all exposed carrier lines to Input with Pull-Down to eliminate leakage
  for (size_t i = 0; i < sizeof(s_module_gpios) / sizeof(s_module_gpios[0]); i++) {
    uint32_t pin = s_module_gpios[i];
    nwy_gpio_direction_set(pin, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(pin, PIN_PULL_PD);
  }
}

static void restore_gpios_after_sleep(void) {
  // Restore Peripheral Master Power (GPIO0 = HIGH)
  nwy_hal_gpio_init_out(PIN_PERIPHERAL_POWER_GPIO0, true);
  nwy_hal_gpio_set_value(PIN_PERIPHERAL_POWER_GPIO0, true);
}

static void print_banner(void) {
  int raw_boot = nwy_pm_boot_res();
  int vbat_mv = 0;
  nwy_hal_pm_get_vbat(&vbat_mv);

  printf("\r\n");
  printf("====================================================================\r\n");
  printf("   NEOWAY N706B: RS-485 + MQTT TELEMETRY + LOW-POWER DEEP SLEEP     \r\n");
  printf("====================================================================\r\n");
  printf("  Hardware: ELEKTRONIKA_N706B_prototype (ASR1605S Cat-1)\r\n");
  printf("  Hardware Revision: 26k Pull-up on Q10/Q1 (Intact Rails)\r\n");
  printf("  Boot Reason: 0x%04X (%s)\r\n", raw_boot, nwy_hal_pm_get_boot_reason_str());
  printf("  Direct VBAT: %d mV\r\n", vbat_mv);
  printf("  ADC Channels: ADC1 (Pin 96, U5 Voltage Div 4.83x) | ADC0 (Pin 9, 60R 4-20mA Loop)\r\n");
  printf("  RS-485 Port: URT2 (MAIN_UART, Pins 17/18) @ %d baud | Direction Pin: GPIO87\r\n", RS485_TEST_BAUD);
  printf("  MQTT Broker: %s:%d (Topic: %s)\r\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_PUB_TOPIC);
  printf("  Sleep Duration: %d seconds per cycle\r\n", SLEEP_DURATION_SEC);
  printf("====================================================================\r\n\r\n");
}

static void main_task(void *param) {
  // Initial core stabilization delay
  nwy_hal_os_thread_sleep(2000);

  // Open UART1 for diagnostic telemetry logging
  g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
  }

  print_banner();

  // Initialize Virtual AT engine
  nwy_error_e at_init = nwy_virt_at_parameter_init();
  LOGI("Virtual AT engine init: %s", (at_init == NWY_SUCCESS) ? "SUCCESS" : "FAIL");

  // Inspect registered UART devices in firmware
  char dev_names[4][NWY_DEV_NAME_MAX_LEN];
  memset(dev_names, 0, sizeof(dev_names));
  int dev_cnt = nwy_dev_name_list_get(NWY_DEV_TYPE_UART, dev_names);
  LOGI("Registered UART Devices in Firmware (%d ports detected):", dev_cnt);
  for (int i = 0; i < dev_cnt; i++) {
    LOGI("  Port [%d]: '%s'", i + 1, dev_names[i]);
  }

  // Inspect registered ADC devices in firmware
  nwy_hal_adc_init();

  // Set modem sleep policy: AT+ENPWRSAVE=1
  char at_resp[64];
  bool enpwr_configured = false;
  for (int retry = 0; retry < 3; retry++) {
    memset(at_resp, 0, sizeof(at_resp));
    if (send_virt_at_cmd("AT+ENPWRSAVE=1", at_resp, sizeof(at_resp))) {
      LOGI("Baseband sleep policy configured: AT+ENPWRSAVE=1 (Attempt %d)", retry + 1);
      enpwr_configured = true;
      break;
    }
    nwy_hal_os_thread_sleep(500);
  }
  if (!enpwr_configured) {
    LOGW("Initial AT+ENPWRSAVE=1 attempt busy; policy will be enforced prior to sleep.");
  }

  uint32_t cycle = 1;

  while (1) {
    LOGI("");
    LOGI("##############################################################");
    LOGI(">>> STARTING BENCHMARK CYCLE %lu <<<", (unsigned long)cycle);
    LOGI("##############################################################");

    // -----------------------------------------------------------------
    // STEP 1: ACTIVE INITIALIZATION & PERIPHERAL POWER ON
    // -----------------------------------------------------------------
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);
    restore_gpios_after_sleep();
    nwy_hal_os_thread_sleep(100);

    int vbat_mv = 0;
    nwy_hal_pm_get_vbat(&vbat_mv);
    LOGI("Cycle %lu: Initial VBAT Rail = %d mV", (unsigned long)cycle, vbat_mv);

    // -----------------------------------------------------------------
    // STAGE 1/5: ANALOG SENSORS & MULTI-CHANNEL ADC TELEMETRY
    // -----------------------------------------------------------------
    LOGI("--- [STAGE 1/5] ANALOG SENSORS & MULTI-CHANNEL ADC TELEMETRY ---");
    LOGI("==================================================================");
    LOGI(">> SAMPLING ONBOARD ANALOG SENSOR INTERFACES <<");
    LOGI("   ADC1 (Module Pin 96, Connector U5): DC Voltage (100k/26.1k divider, 4.831x)");
    LOGI("   ADC0 (Module Pin 9,  Net INADC0):    4-20mA Current Loop (60 ohm shunt)");
    LOGI("   VBAT (Internal Rail):                Direct PMIC Battery Monitoring");
    LOGI("==================================================================");

    nwy_hal_adc_data_t adc_data;
    memset(&adc_data, 0, sizeof(adc_data));
    nwy_hal_adc_sample_all(&adc_data);

    LOGI("ADC1 [DC Voltage Sensor]: Raw Pin = %d mV -> True Vin = %d mV (%.3f V)",
         adc_data.adc1_raw_mv, adc_data.adc1_vin_mv, (float)adc_data.adc1_vin_mv / 1000.0f);
    LOGI("ADC0 [4-20mA Current Loop]: Raw Pin = %d mV -> Loop Current = %.2f mA (%d uA)",
         adc_data.adc0_raw_mv, adc_data.adc0_current_ma, adc_data.adc0_current_ua);
    LOGI("VBAT [Battery Rail via ADC]: %d mV", adc_data.vbat_mv);
    if (adc_data.vbat_mv > 0) {
      vbat_mv = adc_data.vbat_mv;
    }

    // -----------------------------------------------------------------
    // STAGE 2/5: RS-485 TRANSCEIVER TEST (Half-Duplex URT2 / MAIN_UART)
    // -----------------------------------------------------------------
    LOGI("--- [STAGE 2/5] RS-485 HALF-DUPLEX COMMUNICATION TEST ---");
    LOGI("==================================================================");
    LOGI(">> HALF-DUPLEX TIMING SEQUENCE: TX for 10s -> RX for 10s <<");
    LOGI("   Port: URT2 (MAIN_UART, Pins 17/18) @ %d baud, 8-N-1", RS485_TEST_BAUD);
    LOGI("   Direction Control: GPIO87 (HIGH=Transmit, LOW=Receive)");
    LOGI("   Transceiver Power: GPIO00 (Master Peripheral Switch)");
    LOGI("==================================================================");

    char rs485_tx_str[80];
    uint8_t rs485_rx_buf[512];
    int rx_bytes = 0;
    memset(rs485_tx_str, 0, sizeof(rs485_tx_str));
    memset(rs485_rx_buf, 0, sizeof(rs485_rx_buf));

    snprintf(rs485_tx_str, sizeof(rs485_tx_str), "N706B_RS485_CYCLE_%lu_TEST", (unsigned long)cycle);

    if (nwy_hal_rs485_init(RS485_TEST_BAUD)) {
      // 1. TRANSMIT PHASE (10 seconds: 10 bursts spaced 1 second apart)
      LOGI(">>> [PHASE 1/2] TRANSMIT MODE ACTIVE (GPIO87=HIGH) - Sending 10 bursts over 10 seconds...");
      for (int burst = 1; burst <= 10; burst++) {
        char burst_packet[128];
        snprintf(burst_packet, sizeof(burst_packet),
                 "[N706B_RS485_TX_%02d/10] HELLO FROM URT2! CYCLE=%lu BURST=%02d VBAT=%dmV\r\n",
                 burst, (unsigned long)cycle, burst, vbat_mv);
        int written = nwy_hal_rs485_send((const uint8_t *)burst_packet, strlen(burst_packet));
        LOGI("RS-485 TX burst %02d/10: %d bytes sent", burst, written);
        nwy_hal_os_thread_sleep(900); // ~1.0 second per burst
      }

      // 2. RECEIVE PHASE (10 seconds: DE/RE=LOW, polling mode)
      LOGI(">>> [PHASE 2/2] RECEIVE MODE ACTIVE (GPIO87=LOW) - Listening for 10 seconds...");
      rx_bytes = nwy_hal_rs485_listen(rs485_rx_buf, sizeof(rs485_rx_buf), RS485_RX_TIMEOUT_MS);
      if (rx_bytes > 0) {
        LOGI("RS-485 Bus Verified! Received %d bytes", rx_bytes);
        char rx_preview[84];
        int preview_len = (rx_bytes > 80) ? 80 : rx_bytes;
        memcpy(rx_preview, rs485_rx_buf, preview_len);
        rx_preview[preview_len] = '\0';
        LOGI("RS-485 RX Preview (%d bytes): \"%s%s\"",
             rx_bytes, rx_preview, (rx_bytes > 80) ? "..." : "");
      } else {
        LOGW("RS-485 Timeout: No response received within %d ms (Normal if no response sent from PC)", RS485_RX_TIMEOUT_MS);
        snprintf((char *)rs485_rx_buf, sizeof(rs485_rx_buf), "NO_RESPONSE_10S");
      }

      nwy_hal_rs485_deinit();
    } else {
      LOGE("Failed to initialize RS-485 transceiver on URT2!");
      snprintf((char *)rs485_rx_buf, sizeof(rs485_rx_buf), "INIT_FAILED");
    }

    // -----------------------------------------------------------------
    // STEP 4: CELLULAR NETWORK ATTACHMENT & DATA CALL
    // -----------------------------------------------------------------
    LOGI("--- [STAGE 3/5] CELLULAR ATTACHMENT ---");
    bool sim_ready = nwy_hal_sim_is_ready(1);
    LOGI("SIM Card Slot 1 Readiness: %s", sim_ready ? "READY" : "NOT_READY");

    // Wait for cellular network registration (up to 30s)
    bool net_registered = false;
    int csq = 99, ber = 99;
    char oper_name[64] = "Unknown";

    for (int wait_sec = 0; wait_sec < 30; wait_sec++) {
      if (nwy_hal_net_is_registered(1)) {
        net_registered = true;
        break;
      }
      nwy_hal_os_thread_sleep(1000);
    }

    if (net_registered) {
      nwy_hal_net_get_csq(1, &csq, &ber);
      nwy_hal_net_get_operator_name(1, oper_name, sizeof(oper_name));
      LOGI("LTE Registered! Operator: %s, CSQ: %d, BER: %d", oper_name, csq, ber);

      // Start data call on CID 1
      LOGI("Activating cellular data profile (CID 1)...");
      g_data_call_active = false;
      nwy_hal_net_start_data_call(1, 1, data_call_callback);

      // Wait for local IP address allocation (up to 25s, checking callback and direct query)
      for (int ip_wait = 0; ip_wait < 25; ip_wait++) {
        if (g_data_call_active && strlen(g_local_ip) > 0 && strcmp(g_local_ip, "0.0.0.0") != 0) {
          break;
        }
        nwy_data_callinfo_t cinfo;
        memset(&cinfo, 0, sizeof(cinfo));
        if (nwy_data_call_info_get(1, &cinfo) == 0 &&
            strlen(cinfo.ipv4_str) > 0 &&
            strcmp(cinfo.ipv4_str, "0.0.0.0") != 0) {
          strncpy(g_local_ip, cinfo.ipv4_str, sizeof(g_local_ip) - 1);
          g_data_call_active = true;
          LOGI("Active cellular data IP confirmed via direct query: %s", g_local_ip);
          break;
        }
        nwy_hal_os_thread_sleep(1000);
      }
    } else {
      LOGE("Cellular network attachment timed out after 30s!");
    }

    // -----------------------------------------------------------------
    // STEP 5: MQTT TELEMETRY TRANSMISSION
    // -----------------------------------------------------------------
    LOGI("--- [STAGE 4/5] MQTT TELEMETRY STREAM ---");
    if (g_data_call_active && strlen(g_local_ip) > 0) {
      LOGI("Connecting to MQTT Broker (%s:%d)...", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

      if (nwy_hal_mqtt_init(1, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USER, MQTT_PASS, on_mqtt_message_rx)) {
        LOGI("MQTT Connected successfully!");

        // Sanitize RS-485 RX buffer for clean, valid RFC 8259 JSON serialization
        size_t rx_raw_len = (rx_bytes > 0) ? (size_t)rx_bytes : strlen((char *)rs485_rx_buf);
        if (rx_raw_len >= sizeof(rs485_rx_buf)) {
          rx_raw_len = sizeof(rs485_rx_buf) - 1;
        }
        for (size_t i = 0; i < rx_raw_len; i++) {
          unsigned char c = (unsigned char)rs485_rx_buf[i];
          if (c < 32 || c > 126) {
            rs485_rx_buf[i] = ' ';
          } else if (c == '"' || c == '\\') {
            rs485_rx_buf[i] = '\'';
          }
        }
        rs485_rx_buf[rx_raw_len] = '\0';
        // Trim trailing whitespace
        while (rx_raw_len > 0 && rs485_rx_buf[rx_raw_len - 1] == ' ') {
          rs485_rx_buf[--rx_raw_len] = '\0';
        }

        // Construct structured JSON payload with ADC sensor metrics
        char json_payload[1024];
        memset(json_payload, 0, sizeof(json_payload));
        snprintf(json_payload, sizeof(json_payload),
                 "{"
                 "\"device\":\"N706B_ASR1605S\","
                 "\"cycle\":%lu,"
                 "\"vbat_mv\":%d,"
                 "\"csq\":%d,"
                 "\"operator\":\"%s\","
                 "\"ip\":\"%s\","
                 "\"adc1_raw_mv\":%d,"
                 "\"adc1_vin_mv\":%d,"
                 "\"adc0_raw_mv\":%d,"
                 "\"adc0_current_ma\":%.2f,"
                 "\"rs485_tx\":\"%s\","
                 "\"rs485_rx\":\"%s\""
                 "}",
                 (unsigned long)cycle, vbat_mv, csq, oper_name, g_local_ip,
                 adc_data.adc1_raw_mv, adc_data.adc1_vin_mv,
                 adc_data.adc0_raw_mv, adc_data.adc0_current_ma,
                 rs485_tx_str, (char *)rs485_rx_buf);

        LOGI("Publishing JSON payload to '%s' (%u bytes)...", MQTT_PUB_TOPIC, (unsigned int)strlen(json_payload));

        bool pub_ok = nwy_hal_mqtt_publish(MQTT_PUB_TOPIC, json_payload, 1, false);
        LOGI("MQTT Publish Status: %s", pub_ok ? "SUCCESS" : "FAILED");

        // Allow network buffer transmission delay
        nwy_hal_os_thread_sleep(1500);

        LOGI("Disconnecting MQTT session...");
        nwy_hal_mqtt_close();
        nwy_hal_os_thread_sleep(500);
      } else {
        LOGE("Failed to connect to MQTT broker!");
      }
    } else {
      LOGW("Skipping MQTT publish: Cellular data connection inactive or IP 0.0.0.0.");
    }

    // Cleanly deactivate data call if registered
    if (net_registered) {
      LOGI("Deactivating cellular data call (CID 1)...");
      nwy_hal_net_stop_data_call(1, 1);
      // Wait up to 5 seconds for clean PDP context release with the network
      for (int dis_wait = 0; dis_wait < 10; dis_wait++) {
        if (!g_data_call_active) {
          LOGI("Cellular data call deactivated cleanly.");
          break;
        }
        nwy_hal_os_thread_sleep(500);
      }
      nwy_hal_os_thread_sleep(1000);
    }

    // -----------------------------------------------------------------
    // STEP 6: LOW-POWER DEEP SLEEP (30 SECONDS)
    // -----------------------------------------------------------------
    LOGI("");
    LOGI("==============================================================");
    LOGI("--- [STAGE 5/5] ENTERING LOW-POWER DEEP SLEEP (%d SEC) ---", SLEEP_DURATION_SEC);
    LOGI("==============================================================");
    LOGI("Expected PPK2 Floor: ~568.69 uA (26k Pull-up verified benchmark)");

    // 1. Confirm Baseband CP 32kHz clock-gating policy (done while AT engine & SIM are active)
    LOGI("1. Enforcing baseband power save: AT+ENPWRSAVE=1...");
    memset(at_resp, 0, sizeof(at_resp));
    send_virt_at_cmd("AT+ENPWRSAVE=1", at_resp, sizeof(at_resp));
    nwy_hal_os_thread_sleep(200);

    // 2. Minimum functionality mode (CFUN=0 cuts RF synthesizer and starts network detach)
    LOGI("2. Shutting down RF synthesizers: AT+CFUN=0...");
    memset(at_resp, 0, sizeof(at_resp));
    send_virt_at_cmd("AT+CFUN=0", at_resp, sizeof(at_resp));
    // Allow RF synthesizers and baseband protocol stack to reach idle standby cleanly
    nwy_hal_os_thread_sleep(1500);

    // 3. Shut off SIM Card LDO power (done LAST among modem ops so detach is completely done)
    LOGI("3. Cutting USIM LDO power: nwy_sim_power_set(SIM1, 0)...");
    nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 0);
    nwy_hal_os_thread_sleep(300);

    // 4. Isolate all GPIOs (cuts peripheral rail GPIO0 to 0V, pulls signals to GND)
    LOGI("4. Isolating GPIOs and cutting peripheral power rail (GPIO0=0)...");
    isolate_gpios_for_sleep();

    // 5. Authorize Cortex-R5 CPU PMU Sleep
    LOGI("5. Enabling PMU sleep: nwy_pm_state_set(NWY_PM_SLEEP_ENABLE)...");
    nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);

    LOGI("6. Closing UART1 debug logging port...");
    LOGI("--------------------------------------------------------------");
    LOGI("OBSERVE PPK2 CURRENT PROFILE NOW (30-second sleep window)");
    LOGI("--------------------------------------------------------------");
    nwy_hal_os_thread_sleep(200);

    if (g_dbg_uart_fd >= 0) {
      nwy_hal_uart_close(g_dbg_uart_fd);
      g_dbg_uart_fd = -1;
      nwy_hal_uart_set_log_fd(-1);
    }

    // Enter deep sleep for 30 seconds
    nwy_hal_os_thread_sleep(SLEEP_DURATION_SEC * 1000);

    // -----------------------------------------------------------------
    // STEP 6: WAKEUP & RESTORATION
    // -----------------------------------------------------------------
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
    if (g_dbg_uart_fd >= 0) {
      nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
    }

    LOGI("");
    LOGI("==============================================================");
    LOGI(">>> CYCLE %lu COMPLETE - CPU WAKEUP RESUMED! <<<", (unsigned long)cycle);
    LOGI("==============================================================");

    // Restore peripheral master power, USIM LDO, and RF synthesizers
    LOGI("Restoring peripheral power, USIM LDO, and RF (AT+CFUN=1)...");
    restore_gpios_after_sleep();

    // Power up USIM LDO and wait for SIM ATR (Answer-to-Reset) before sending AT+CFUN=1
    nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 1);
    nwy_hal_os_thread_sleep(1000);

    memset(at_resp, 0, sizeof(at_resp));
    send_virt_at_cmd("AT+CFUN=1", at_resp, sizeof(at_resp));

    // Recovery stabilization
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
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_close(g_dbg_uart_fd);
  }
}
