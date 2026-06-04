#include "hal/nwy_hal_mqtt.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h"

#include "MQTTClient.h"
#include "MQTTFreeRTOS.h"
#include <string.h>

#define TAG "HAL_MQTT"
#define MQTT_BUFF_LIMIT 512

static Network g_network;
static MQTTClient g_client;
static unsigned char g_tx_buf[MQTT_BUFF_LIMIT];
static unsigned char g_rx_buf[MQTT_BUFF_LIMIT];

static nwy_osi_thread_t g_yield_thread = NULL;
static volatile bool g_loop_active = false;
static nwy_hal_mqtt_cb_t g_user_cb = NULL;

static void internal_paho_message_handler(MessageData *md) {
  if (!md || !md->message || !md->topicName || !g_user_cb)
    return;

  // Safely extract the topic string (Paho MQTTString isn't always
  // null-terminated)
  char topic_buf[128];
  int t_len = md->topicName->lenstring.len;
  if (t_len >= sizeof(topic_buf))
    t_len = sizeof(topic_buf) - 1;
  memcpy(topic_buf, md->topicName->lenstring.data, t_len);
  topic_buf[t_len] = '\0';

  g_user_cb(topic_buf, (const uint8_t *)md->message->payload,
            md->message->payloadlen);
}

static void mqtt_background_yield_task(void *param) {
  nwy_hal_uart_log_i(TAG, "MQTT asynchronous yield pump thread active.");
  while (g_loop_active) {
    // Keep-alive ping and message processor
    if (MQTTYield(&g_client, 1000) != 0) {
      nwy_hal_uart_log_e(TAG, "Yield pump detected broker disconnection.");
      break;
    }
    nwy_hal_os_thread_sleep(100);
  }
  g_loop_active = false;
  NetworkDisconnect(&g_network);
  nwy_hal_uart_log_w(TAG, "MQTT Loop exited.");
}

bool nwy_hal_mqtt_init(uint16_t cid, const char *host, uint16_t port,
                       const char *user, const char *pass,
                       nwy_hal_mqtt_cb_t cb) {
  if (!host || port == 0)
    return false;

  nwy_hal_mqtt_close();
  g_user_cb = cb;

  // 1. Initialize Network and map to cellular data context
  NetworkInit(&g_network);
  g_network.cid = cid;  // CRITICAL: Tells the modem to route via SIM1 profile!
  g_network.is_SSL = 0; // 0 = Standard TCP for Port 1883

  MQTTClientInit(&g_client, &g_network, 10000, g_tx_buf, sizeof(g_tx_buf),
                 g_rx_buf, sizeof(g_rx_buf));

  nwy_hal_uart_log_i(TAG,
                     "Routing downstream socket pipeline -> %s:%u (CID: %d)",
                     host, port, cid);

  // 2. Open TCP Socket
  int net_status = NetworkConnect(&g_network, (char *)host, port);

  // BUGFIX: Neoway returns the Socket FD (>0) on success, not 0.
  if (net_status < 0) {
    nwy_hal_uart_log_e(TAG, "Failed to connect to broker via TCP. Status: %d",
                       net_status);
    return false;
  }
  nwy_hal_uart_log_i(TAG, "TCP Handshake Success. (Descriptor: %d)",
                     net_status);

  // 3. Connect to MQTT Protocol
  MQTTPacket_connectData opts = MQTTPacket_connectData_initializer;
  opts.MQTTVersion = 4; // v3.1.1
  opts.clientID.cstring = "N706B_Device_01";
  opts.keepAliveInterval = 60;
  opts.cleansession = 1;

  if (user)
    opts.username.cstring = (char *)user;
  if (pass)
    opts.password.cstring = (char *)pass;

  nwy_hal_uart_log_i(TAG, "Sending protocol connection token...");
  int mqtt_status = MQTTConnect(&g_client, &opts);
  if (mqtt_status != 0) {
    nwy_hal_uart_log_e(TAG,
                       "Broker rejected connection authorization. Status: %d",
                       mqtt_status);
    NetworkDisconnect(&g_network);
    return false;
  }

  // 4. Start Background Thread
  g_loop_active = true;
  if (!nwy_hal_os_thread_create(&g_yield_thread, "mqtt_pump",
                                mqtt_background_yield_task, NULL,
                                NWY_OSI_PRIORITY_NORMAL, 1024 * 8)) {
    g_loop_active = false;
    MQTTDisconnect(&g_client);
    NetworkDisconnect(&g_network);
    nwy_hal_uart_log_e(TAG, "Failed to create yield thread.");
    return false;
  }

  nwy_hal_uart_log_i(TAG, "MQTT Connection is fully operational.");
  return true;
}

bool nwy_hal_mqtt_subscribe(const char *topic, int qos, nwy_hal_mqtt_cb_t cb) {
  if (!g_loop_active || !topic)
    return false;
  if (cb)
    g_user_cb = cb;
  return (MQTTSubscribe(&g_client, topic, qos, internal_paho_message_handler) ==
          0);
}

bool nwy_hal_mqtt_publish(const char *topic, const char *payload, int qos,
                          bool retain) {
  if (!g_loop_active || !topic || !payload)
    return false;

  MQTTMessage msg;
  msg.qos = qos;
  msg.retained = retain ? 1 : 0;
  msg.dup = 0;
  msg.payload = (void *)payload;
  msg.payloadlen = strlen(payload);

  return (MQTTPublish(&g_client, topic, &msg) == 0);
}

void nwy_hal_mqtt_close(void) {
  if (g_loop_active) {
    g_loop_active = false;
    MQTTDisconnect(&g_client);
    nwy_hal_os_thread_sleep(200);
  }
}