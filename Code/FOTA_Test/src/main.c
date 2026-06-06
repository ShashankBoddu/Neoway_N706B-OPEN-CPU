#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_mqtt.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_secure_fota.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_sms.h"
#include "hal/nwy_hal_sntp.h"
#include "hal/nwy_hal_socket.h"
#include "hal/nwy_hal_uart.h"

#include "nwy_data_api.h"
#include "nwy_http_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "NetMonitor"
#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;
#define printf(fmt, ...) nwy_hal_uart_printf(g_uart_fd, fmt, ##__VA_ARGS__)

#define MQTT_BROKER_HOST "mqtt.fervidlabs.in"
#define MQTT_BROKER_PORT 1883
#define MQTT_USER "fervid"
#define MQTT_PASS "Fervid@123"

typedef enum {
  NET_LED_STATE_OOS = 0,
  NET_LED_STATE_REGISTERED,
  NET_LED_STATE_CONNECTED
} net_led_state_e;

static volatile net_led_state_e g_net_led_state = NET_LED_STATE_OOS;

// Extract Value from JSON string safely
static bool extract_json_str(const char *json, const char *key, char *out,
                             int max_len) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  char *start = strstr(json, search);
  if (!start)
    return false;
  start += strlen(search);
  char *end = strchr(start, '\"');
  if (!end || (end - start) >= max_len)
    return false;
  strncpy(out, start, end - start);
  out[end - start] = '\0';
  return true;
}

static bool extract_json_int(const char *json, const char *key, uint32_t *out) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":", key);
  char *start = strstr(json, search);
  if (!start)
    return false;
  start += strlen(search);
  *out = (uint32_t)atoi(start);
  return true;
}

static void on_mqtt_message(const char *topic, const uint8_t *payload,
                            uint32_t len) {
  char *msg = (char *)malloc(len + 1);
  if (!msg)
    return;
  memcpy(msg, payload, len);
  msg[len] = '\0';

  LOGI("MQTT RX [%s]: %s", topic, msg);

  if (strstr(msg, "\"command\":\"OTA\"") ||
      strstr(msg, "\"command\": \"OTA\"")) {
    char url[256] = {0};
    char signature[150] = {0};
    uint32_t size = 0;

    bool has_url = extract_json_str(msg, "OTAConfigURL", url, sizeof(url));
    bool has_sig =
        extract_json_str(msg, "signature", signature, sizeof(signature));
    bool has_size = extract_json_int(msg, "Configsize", &size);

    if (has_url && has_sig && has_size) {
      LOGI("Valid OTA Trigger Found! Initiating Secure Download...");
      nwy_hal_secure_fota_start(url, size, signature);
    } else {
      LOGE("Malformed OTA Trigger. Missing URL, Signature, or Size.");
    }
  }
  free(msg);
}

static void execute_automated_mqtt_flow(void) {
  LOGI("Initializing Secure IoT Framework...");
  if (nwy_hal_mqtt_init(1, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USER,
                        MQTT_PASS, on_mqtt_message)) {
    LOGI("MQTT Connected! Deploying application subscriptions...");
    nwy_hal_mqtt_subscribe("nwy_n706b/fervid/rx", 0, on_mqtt_message);
    nwy_hal_mqtt_publish("nwy_n706b/fervid/tx",
                         "{\"status\":\"online_ready_for_ota\"}", 0, false);
  } else {
    LOGE("MQTT Init Failed.");
  }
}

static void test_sntp_cb(bool success) { execute_automated_mqtt_flow(); }

static void test_data_call_cb(int sim_id, int profile_idx, bool connected,
                              const char *ip_address) {
  if (connected) {
    LOGI("DATA CALL CONNECTED IP: %s", ip_address ? ip_address : "N/A");
    nwy_hal_sntp_sync_time(1, "pool.ntp.org", "E5", test_sntp_cb);
  }
}

static void network_monitor_task(void *param) {
  nwy_hal_os_thread_sleep(3000);
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd >= 0)
    nwy_hal_uart_set_log_fd(g_uart_fd);

  printf("\r\n==================================================\r\n");
  printf("   NEOWAY N706B OPENCPU - SECURE FOTA ENGINE V1      \r\n");
  printf("==================================================\r\n");

  nwy_hal_gpio_init_out(HAL_GPIO_STATUS, true);
  nwy_hal_gpio_init_out(HAL_GPIO_NET_STATUS, false);
  nwy_hal_net_set_mode(1, NWY_NW_MODE_MASK_LTE);

  bool data_call_dialed = false;
  while (1) {
    nwy_hal_os_thread_sleep(5000);
    bool sim_ready = nwy_hal_sim_is_ready(1);
    int cs_state = 0, ps_state = 0, rat = 0;
    nwy_hal_net_get_registration_details(1, &cs_state, &ps_state, &rat);

    char ip_address[32] = "N/A";
    bool ip_ok = nwy_hal_net_get_ip(1, 1, ip_address, sizeof(ip_address));

    if (ip_ok) {
      g_net_led_state = NET_LED_STATE_CONNECTED;
    } else if (sim_ready && (ps_state == NWY_NW_SERVICE_FULL ||
                             ps_state == NWY_NW_SERVICE_LIMITED)) {
      g_net_led_state = NET_LED_STATE_REGISTERED;
    } else {
      g_net_led_state = NET_LED_STATE_OOS;
    }

    if (sim_ready &&
        (ps_state == NWY_NW_SERVICE_FULL ||
         ps_state == NWY_NW_SERVICE_LIMITED) &&
        !ip_ok) {
      if (!data_call_dialed) {
        nwy_hal_net_start_data_call(1, 1, test_data_call_cb);
        data_call_dialed = true;
      }
    } else if (ps_state == NWY_NW_SERVICE_NONE || !sim_ready) {
      data_call_dialed = false;
    }
  }
}

static void led_indicator_task(void *param) {
  while (1) {
    switch (g_net_led_state) {
    case NET_LED_STATE_OOS:
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, true);
      nwy_hal_os_thread_sleep(100);
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, false);
      nwy_hal_os_thread_sleep(100);
      break;
    case NET_LED_STATE_REGISTERED:
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, true);
      nwy_hal_os_thread_sleep(200);
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, false);
      nwy_hal_os_thread_sleep(1800);
      break;
    case NET_LED_STATE_CONNECTED:
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, true);
      nwy_hal_os_thread_sleep(100);
      nwy_hal_gpio_set_value(HAL_GPIO_NET_STATUS, false);
      nwy_hal_os_thread_sleep(2000);
      break;
    }
  }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_osi_thread_t monitor_thread = NULL;
  nwy_hal_os_thread_create(&monitor_thread, "net_monitor", network_monitor_task,
                           NULL, NWY_OSI_PRIORITY_NORMAL, 1024 * 16);

  nwy_osi_thread_t led_thread = NULL;
  nwy_hal_os_thread_create(&led_thread, "led_indicator", led_indicator_task,
                           NULL, NWY_OSI_PRIORITY_NORMAL, 1024 * 2);

  return 0;
}

void appimg_exit(void) {
  if (g_uart_fd >= 0) {
    nwy_hal_sim_unregister_urc_cb(1);
    nwy_hal_net_stop_data_call(1, 1);
    nwy_hal_uart_close(g_uart_fd);
  }
}