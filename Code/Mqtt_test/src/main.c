#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_sms.h"
#include "hal/nwy_hal_sntp.h"
#include "hal/nwy_hal_uart.h"
#include "nwy_data_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_socket_api.h"

// NATIVE PAHO EMBEDDED MQTT STRUCT REFERENCE HEADERS
#include "MQTTClient.h"
#include "nwy_pahomqtt_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  NET_LED_STATE_OOS = 0,
  NET_LED_STATE_REGISTERED,
  NET_LED_STATE_CONNECTED
} net_led_state_e;

static volatile net_led_state_e g_net_led_state = NET_LED_STATE_OOS;

#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200
#define TAG "NetMonitor"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;
#define printf(fmt, ...) nwy_hal_uart_printf(g_uart_fd, fmt, ##__VA_ARGS__)

// ============================================================================
// MQTT CONFIGURATION PLATFORM MATRIX
// ============================================================================
#define MQTT_BROKER_HOST "mqtt.fervidlabs.in"
#define MQTT_BROKER_USER "fervid"
#define MQTT_BROKER_PASS "Fervid@123"
#define MQTT_BROKER_PORT 1883
#define MQTT_TOPIC_SUB "nwy_n706b/fervid/rx"
#define MQTT_TOPIC_PUB "nwy_n706b/fervid/tx"

static Network g_mqtt_network;
static MQTTClient g_mqtt_client;
static unsigned char g_mqtt_tx_buf[512];
static unsigned char g_mqtt_rx_buf[512];
static nwy_osi_thread_t g_mqtt_yield_task = NULL;
static volatile bool g_mqtt_loop_running = false;

/**
 * @brief Asynchronous Subscription Incoming Packet Callback Handler.
 */
static void mqtt_message_arrived_cb(MessageData *data) {
  if (!data || !data->message || !data->topicName)
    return;

  uint32_t topic_len = data->topicName->lenstring.len;
  uint32_t payload_len = data->message->payloadlen;

  char *topic_str = (char *)malloc(topic_len + 1);
  char *payload_str = (char *)malloc(payload_len + 1);

  if (topic_str && payload_str) {
    memcpy(topic_str, data->topicName->lenstring.data, topic_len);
    topic_str[topic_len] = '\0';

    memcpy(payload_str, data->message->payload, payload_len);
    payload_str[payload_len] = '\0';

    printf("\r\n--- [INBOUND MQTT PACKET DETECTED] ---\r\n");
    printf("Topic Target: %s\r\n", topic_str);
    printf("Message Payload: %s\r\n", payload_str);
    printf("QoS Verification: %d\r\n", data->message->qos);
    printf("----------------------------------------\r\n");
  }

  if (topic_str)
    free(topic_str);
  if (payload_str)
    free(payload_str);
}

/**
 * @brief Thread Background Worker Task to feed Keepalive metrics
 * (PingReq/PingResp).
 */
static void mqtt_yield_task_worker(void *param) {
  LOGI("MQTT_THREAD: Background yield pipeline established active monitoring "
       "loop.");

  while (g_mqtt_loop_running) {
    // Core background parsing pump required to maintain network status
    int yield_res =
        MQTTYield(&g_mqtt_client, 1000); // 1-second packet check window
    if (yield_res != 0) {
      LOGE("MQTT_THREAD: Active broker link dropped. Internal error response: "
           "%d",
           yield_res);
      break;
    }
    nwy_hal_os_thread_sleep(200);
  }

  g_mqtt_loop_running = false;
  NetworkDisconnect(&g_mqtt_network); //
  LOGW(
      "MQTT_THREAD: Worker deactivated. Network descriptors released cleanly.");
}

/**
 * @brief Automated MQTT Authentication, Handshake and Topology Sequence.
 */
static void execute_automated_mqtt_flow(void) {
  printf("\r\n");
  printf("==================================================\r\n");
  printf("     INDUSTRIAL STEP 6: SECURE PAHO MQTT TRIGGER   \r\n");
  printf("==================================================\r\n");
  LOGI("STAGE 6 MQTT: Constructing context memory maps...");

  // 1. Core Network Layer Allocation
  NetworkInit(&g_mqtt_network);
  g_mqtt_network.my_socket =
      nwy_socket_open(2, 1, 6, 1); // AF_INET=2, SOCK_STREAM=1, TCP=6
  if (g_mqtt_network.my_socket < 0) {
    LOGE("STAGE 6 MQTT: Core driver flatly rejected socket initialization.");
    return;
  }

  // 2. Client Parameter Map Bindings
  MQTTClientInit(&g_mqtt_client, &g_mqtt_network, 10000, g_mqtt_tx_buf,
                 sizeof(g_mqtt_tx_buf), g_mqtt_rx_buf, sizeof(g_mqtt_rx_buf));

  LOGI("STAGE 6 MQTT: Dialing connection endpoint -> %s:%d", MQTT_BROKER_HOST,
       MQTT_BROKER_PORT);
  int net_link_res =
      NetworkConnect(&g_mqtt_network, MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  if (net_link_res != 0) {
    LOGE("STAGE 6 MQTT: Socket routing failure on broker connection path: %d",
         net_link_res);
    nwy_socket_close(g_mqtt_network.my_socket);
    return;
  }
  LOGI("STAGE 6 MQTT: TCP layer mapped. Assembling authentication token "
       "frame...");

  // 3. Mount Parameter Arrays with Security Settings
  MQTTPacket_connectData auth_connection_data =
      MQTTPacket_connectData_initializer;
  auth_connection_data.MQTTVersion = 4; // Use standard v3.1.1 (Value 4)
  auth_connection_data.clientID.cstring = "N706B_Fervid_Device_01";
  auth_connection_data.keepAliveInterval = 60; // 60-second ping cycles
  auth_connection_data.cleansession = 1;

  // SECURE AUTHENTICATION ASSIGNMENTS VIA EXPLICIT POINTER STRING VALUES
  auth_connection_data.username.cstring = MQTT_BROKER_USER;
  auth_connection_data.password.cstring = MQTT_BROKER_PASS;

  LOGI("STAGE 6 MQTT: Transferring authentication envelope to FervidLabs "
       "Broker...");
  int protocol_handshake_res =
      MQTTConnect(&g_mqtt_client, &auth_connection_data);
  if (protocol_handshake_res != 0) { // 0 represents Connection Accepted status
                                     // in standard Paho implementations
    LOGE("STAGE 6 MQTT: Protocol authentication rejected by broker. Code: %d",
         protocol_handshake_res);
    NetworkDisconnect(&g_mqtt_network); //
    return;
  }
  LOGI("STAGE 6 MQTT: [SUCCESS] Verified link online. Spawning keepalive "
       "thread.");

  // 4. Background Engine Task Instantiation
  g_mqtt_loop_running = true;
  bool system_task_online = nwy_hal_os_thread_create(
      &g_mqtt_yield_task, "mqtt_yielder", mqtt_yield_task_worker, NULL,
      NWY_OSI_PRIORITY_NORMAL, 1024 * 8);
  if (!system_task_online) {
    LOGE("STAGE 6 MQTT: Failed to initialize yield processing worker.");
    g_mqtt_loop_running = false;
    NetworkDisconnect(&g_mqtt_network); //
    return;
  }

  // 5. Subscribe to Controlled Input Gate
  LOGI("STAGE 6 MQTT: Binding subscription matrix path: %s", MQTT_TOPIC_SUB);
  int sub_status = MQTTSubscribe(&g_mqtt_client, MQTT_TOPIC_SUB, QOS0,
                                 mqtt_message_arrived_cb); //
  if (sub_status != 0) {
    LOGW("STAGE 6 MQTT: Subscription tracking configuration code failure: %d",
         sub_status);
  }

  // 6. Transmit Initial Diagnostics Payload Frame
  LOGI(
      "STAGE 6 MQTT: Delivering initial active packet payload data to path: %s",
      MQTT_TOPIC_PUB);
  char telemetry_packet[160];
  snprintf(telemetry_packet, sizeof(telemetry_packet),
           "{\"dev\":\"N706B\",\"auth\":\"authenticated\",\"topic\":\"fervid_"
           "labs\",\"status\":\"active\"}");

  MQTTMessage operational_message;
  operational_message.qos = QOS0;
  operational_message.retained = 0;
  operational_message.dup = 0;
  operational_message.payload = (void *)telemetry_packet;
  operational_message.payloadlen = strlen(telemetry_packet);

  int publish_res =
      MQTTPublish(&g_mqtt_client, MQTT_TOPIC_PUB, &operational_message);
  if (publish_res != 0) {
    LOGE("STAGE 6 MQTT: Telemetry transmission pipeline dropped. Code: %d",
         publish_res);
  } else {
    LOGI("STAGE 6 MQTT: Telemetry frame dropped cleanly to host routing "
         "buffers.");
  }
  printf("==================================================\r\n");
}

// ============================================================================
// REST OF SYSTEM DRIVER HOOKS (DO NOT ALTER BASE ARCHITECTURE MAPS)
// ============================================================================
static const char *get_reg_state_str(int state) {
  switch (state) {
  case NWY_NW_SERVICE_NONE:
    return "OUT OF SERVICE (0)";
  case NWY_NW_SERVICE_LIMITED:
    return "LIMITED SERVICE (1)";
  case NWY_NW_SERVICE_FULL:
    return "FULL SERVICE (2)";
  default:
    return "UNKNOWN";
  }
}

static const char *get_rat_str(int rat) {
  switch (rat) {
  case NWY_NW_RAT_NONE:
    return "NONE";
  case NWY_NW_RAT_GSM:
    return "GSM (2G)";
  case NWY_NW_RAT_WCDMA:
    return "WCDMA (3G)";
  case NWY_NW_RAT_LTE:
    return "LTE (4G)";
  default:
    return "UNKNOWN";
  }
}

static void get_net_mode_str(int mode, char *buf, int max_len) {
  if (mode == NWY_NW_MODE_MASK_AUTO) {
    snprintf(buf, max_len, "AUTO (unlocked)");
    return;
  }
  buf[0] = '\0';
  int len = 0;
  if (mode & NWY_NW_MODE_MASK_GSM)
    len += snprintf(buf + len, max_len - len, "GSM ");
  if (mode & NWY_NW_MODE_MASK_LTE)
    len += snprintf(buf + len, max_len - len, "LTE ");
  if (len > 0 && buf[len - 1] == ' ')
    buf[len - 1] = '\0';
}

static int csq_to_dbm(int csq) {
  if (csq == 99)
    return 0;
  return -113 + (csq * 2);
}

static void test_sim_urc_cb(int sim_id, bool card_present,
                            nwy_sim_status_e sim_status) {
  LOGI("SIM EVENT - SIM %d: Present=%s, Status=%d", sim_id,
       card_present ? "YES" : "NO", (int)sim_status);
}

static void test_sntp_cb(bool success) {
  if (success) {
    LOGI("SNTP TIME SYNC: [SUCCESS] System clock aligned over cellular UDP.");
  } else {
    LOGW("SNTP TIME SYNC: [FAILED] Clock window timeout. Processing context "
         "pipeline regardless...");
  }
  // Transition directly to authenticated MQTT Operations
  execute_automated_mqtt_flow();
}

static void test_data_call_cb(int sim_id, int profile_idx, bool connected,
                              const char *ip_address) {
  if (connected) {
    LOGI("DATA CALL CONNECTED - SIM %d, Profile %d, IP: %s", sim_id,
         profile_idx, ip_address ? ip_address : "N/A");
    LOGI("Triggering SNTP time synchronization over cellular network...");
    nwy_hal_sntp_sync_time(1, "pool.ntp.org", "E5", test_sntp_cb);
  } else {
    LOGW("DATA CALL DISCONNECTED - SIM %d, Profile %d", sim_id, profile_idx);
  }
}

static void run_network_diagnostics_scan(void) {
  printf("\r\n");
  printf("==================================================\r\n");
  printf("         NETWORK HAL DIAGNOSTIC SELF-TEST REPORT  \r\n");
  printf("==================================================\r\n");

  int target_sim = 1;
  bool radio_restart_needed = false;

  if (!nwy_hal_sim_is_ready(target_sim)) {
    LOGE("SIM STATUS: [CRITICAL] SIM card is NOT DETECTED or NOT READY!");
  } else {
    LOGI("SIM STATUS: [OK] SIM Card verified ready.");
  }

  int net_mode = 0;
  if (nwy_hal_net_get_mode(target_sim, &net_mode)) {
    char net_mode_buf[64];
    get_net_mode_str(net_mode, net_mode_buf, sizeof(net_mode_buf));
    LOGI("Preferred Network RAT Mode: %s (0x%X)", net_mode_buf, net_mode);
    if (net_mode != NWY_NW_MODE_MASK_LTE) {
      if (nwy_hal_net_set_mode(target_sim, NWY_NW_MODE_MASK_LTE)) {
        LOGI("Network Mode locked to LTE successfully.");
        radio_restart_needed = true;
      }
    }
  }

  int cs_state = 0, ps_state = 0, rat = 0;
  if (nwy_hal_net_get_registration_details(target_sim, &cs_state, &ps_state,
                                           &rat)) {
    LOGI("CS Domain Registration: %s", get_reg_state_str(cs_state));
    LOGI("PS Domain Registration: %s", get_reg_state_str(ps_state));
    LOGI("Active Radio Technology: %s", get_rat_str(rat));
  }

  int csq = 99, ber = 99;
  if (nwy_hal_net_get_csq(target_sim, &csq, &ber)) {
    LOGI("SIGNAL STATUS: CSQ: %d (%d dBm)", csq, csq_to_dbm(csq));
  }
  printf("==================================================\r\n");

  if (radio_restart_needed) {
    nwy_hal_net_set_radio_mode(NWY_NW_RADIO_FLIGHT_MODE);
    nwy_hal_os_thread_sleep(2000);
    nwy_hal_net_set_radio_mode(NWY_NW_RADIO_NORMAL_MODE);
  }
}

static void network_monitor_task(void *param) {
  nwy_hal_os_thread_sleep(3000);
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd < 0)
    return;

  nwy_hal_uart_set_log_fd(g_uart_fd);

  printf("\r\n==================================================\r\n");
  printf("   NEOWAY N706B OPENCPU - NETWORK MONITOR TEST 2   \r\n");
  printf("==================================================\r\n");

  nwy_hal_gpio_init_out(HAL_GPIO_STATUS, true);
  nwy_hal_gpio_init_out(HAL_GPIO_NET_STATUS, false);
  nwy_hal_sim_set_detect(1, 0, 0);
  nwy_hal_sim_register_urc_cb(1, test_sim_urc_cb);

  run_network_diagnostics_scan();
  nwy_hal_sms_init(1, NWY_SMS_STORAGE_TYPE_NV);

  bool data_call_dialed = false;

  while (1) {
    nwy_hal_os_thread_sleep(10000);
    nwy_time_t current_time;
    int timezone = 0;

    if (nwy_hal_pm_get_time(&current_time, &timezone)) {
      printf("\r\n--- TIMESTAMP: %04d-%02d-%02d %02d:%02d:%02d ---\r\n",
             current_time.year, current_time.mon, current_time.day,
             current_time.hour, current_time.min, current_time.sec);
    }

    bool sim_ready = nwy_hal_sim_is_ready(1);
    int cs_state = 0, ps_state = 0, rat = 0;
    nwy_hal_net_get_registration_details(1, &cs_state, &ps_state, &rat);

    char ip_address[32] = "N/A";
    bool ip_ok = nwy_hal_net_get_ip(1, 1, ip_address, sizeof(ip_address));

    if (ip_ok) {
      LOGI("[STATUS] INTERNET: CONNECTED (IP: %s)", ip_address);
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
        LOGI("Network registered. Launching GPRS PDP Context Activation...");
        bool dial_ok = nwy_hal_net_start_data_call(1, 1, test_data_call_cb);
        LOGI("GPRS Activation request status: %s",
             dial_ok ? "SUCCESS" : "FAILED");
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
  nwy_thread_sleep(1000);
  nwy_osi_thread_t monitor_thread = NULL;

  // Implemented safe 16KB execution boundaries to allow simultaneous FreeRTOS
  // stack calls
  nwy_hal_os_thread_create(&monitor_thread, "net_monitor", network_monitor_task,
                           NULL, NWY_OSI_PRIORITY_NORMAL, 1024 * 16);

  nwy_osi_thread_t led_thread = NULL;
  nwy_hal_os_thread_create(&led_thread, "led_indicator", led_indicator_task,
                           NULL, NWY_OSI_PRIORITY_NORMAL, 1024 * 2);

  return 0;
}

void appimg_exit(void) {
  if (g_uart_fd >= 0) {
    g_mqtt_loop_running = false;
    nwy_hal_sim_unregister_urc_cb(1);
    nwy_hal_net_stop_data_call(1, 1);
    nwy_hal_uart_close(g_uart_fd);
  }
}