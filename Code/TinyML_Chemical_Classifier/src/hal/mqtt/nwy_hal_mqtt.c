#include "hal/nwy_hal_mqtt.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/uart/nwy_hal_uart.h"

#include "MQTTClient.h"
#include "MQTTFreeRTOS.h"
#include "nwy_osi_api.h"
#include <string.h>

#define TAG "HAL_MQTT"
#define MQTT_BUFF_LIMIT 512

static Network g_network;
static MQTTClient g_client;
static unsigned char g_tx_buf[MQTT_BUFF_LIMIT];
static unsigned char g_rx_buf[MQTT_BUFF_LIMIT];

static nwy_osi_thread_t g_yield_thread = NULL;
static volatile bool g_loop_active = false;
static volatile bool g_yield_thread_running = false;
static nwy_hal_mqtt_cb_t g_user_cb = NULL;

/* Mutex protecting all Paho MQTTClient API access (thread-safe industrial standard) */
static nwy_osi_mutex_t g_mqtt_mutex = NULL;

static void internal_paho_message_handler(MessageData *md) {
    if (!md || !md->message || !md->topicName || !g_user_cb)
        return;

    char topic_buf[128];
    int t_len = md->topicName->lenstring.len;
    if (t_len >= (int)sizeof(topic_buf))
        t_len = (int)sizeof(topic_buf) - 1;
    memcpy(topic_buf, md->topicName->lenstring.data, t_len);
    topic_buf[t_len] = '\0';

    /* Deliver message to upper layer callback (must not block or re-enter MQTT publish) */
    g_user_cb(topic_buf, (const uint8_t *)md->message->payload,
              md->message->payloadlen);
}

static void mqtt_background_yield_task(void *param) {
    (void)param;
    g_yield_thread_running = true;
    nwy_hal_uart_log_i(TAG, "MQTT yield pump thread active.");

    while (g_loop_active) {
        int rc = 0;

        if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 200);
        if (g_loop_active) {
            rc = MQTTYield(&g_client, 200);
        }
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);

        if (rc != 0) {
            nwy_hal_uart_log_w(TAG, "Yield pump detected error or disconnection (rc: %d).", rc);
            break;
        }

        nwy_hal_os_thread_sleep(50);
    }

    g_loop_active = false;
    g_yield_thread_running = false;

    if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 500);
    NetworkDisconnect(&g_network);
    if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);

    nwy_hal_uart_log_w(TAG, "MQTT yield pump loop exited.");
    nwy_thread_exit(NULL);
}

bool nwy_hal_mqtt_init(uint16_t cid, const char *host, uint16_t port,
                       const char *user, const char *pass,
                       nwy_hal_mqtt_cb_t cb) {
    if (!host || port == 0)
        return false;

    if (!g_mqtt_mutex) {
        nwy_sdk_mutex_create(&g_mqtt_mutex);
    }

    nwy_hal_mqtt_close();
    g_user_cb = cb;

    if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 2000);

    // 1. Initialize Network and map to cellular data context
    NetworkInit(&g_network);
    g_network.cid = cid;  // Tells modem to route via SIM1 profile
    g_network.is_SSL = 0; // Standard TCP for Port 1883

    MQTTClientInit(&g_client, &g_network, 10000, g_tx_buf, sizeof(g_tx_buf),
                   g_rx_buf, sizeof(g_rx_buf));

    nwy_hal_uart_log_i(TAG, "Routing downstream socket pipeline -> %s:%u (CID: %d)",
                       host, port, cid);

    // 2. Open TCP Socket
    int net_status = NetworkConnect(&g_network, (char *)host, port);
    if (net_status < 0) {
        nwy_hal_uart_log_e(TAG, "Failed to connect to broker via TCP. Status: %d",
                           net_status);
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);
        return false;
    }
    nwy_hal_uart_log_i(TAG, "TCP Handshake Success. (Descriptor: %d)", net_status);

    // 3. Connect to MQTT Protocol
    MQTTPacket_connectData opts = MQTTPacket_connectData_initializer;
    opts.MQTTVersion = 4; // v3.1.1
    opts.clientID.cstring = "N706B_BME688_01";
    opts.keepAliveInterval = 60;
    opts.cleansession = 1;

    if (user)
        opts.username.cstring = (char *)user;
    if (pass)
        opts.password.cstring = (char *)pass;

    nwy_hal_uart_log_i(TAG, "Sending MQTT protocol connection token...");
    int mqtt_status = MQTTConnect(&g_client, &opts);
    if (mqtt_status != 0) {
        nwy_hal_uart_log_e(TAG, "Broker rejected connection authorization. Status: %d",
                           mqtt_status);
        NetworkDisconnect(&g_network);
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);
        return false;
    }

    if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);

    // 4. Start Background Yield Thread
    g_loop_active = true;
    if (!nwy_hal_os_thread_create(&g_yield_thread, "mqtt_pump",
                                  mqtt_background_yield_task, NULL,
                                  NWY_OSI_PRIORITY_NORMAL, 1024 * 8)) {
        g_loop_active = false;
        if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 1000);
        MQTTDisconnect(&g_client);
        NetworkDisconnect(&g_network);
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);
        nwy_hal_uart_log_e(TAG, "Failed to create yield thread.");
        return false;
    }

    nwy_hal_uart_log_i(TAG, "MQTT connection is fully operational.");
    return true;
}

bool nwy_hal_mqtt_subscribe(const char *topic, int qos, nwy_hal_mqtt_cb_t cb) {
    if (!g_loop_active || !topic)
        return false;

    if (cb)
        g_user_cb = cb;

    int rc = -1;
    if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 2000);
    if (g_loop_active) {
        rc = MQTTSubscribe(&g_client, topic, qos, internal_paho_message_handler);
    }
    if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);

    return (rc == 0);
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

    int rc = -1;
    if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 2000);
    if (g_loop_active) {
        rc = MQTTPublish(&g_client, topic, &msg);
    }
    if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);

    return (rc == 0);
}

bool nwy_hal_mqtt_is_connected(void) {
    return g_loop_active && g_yield_thread_running;
}

void nwy_hal_mqtt_close(void) {
    if (g_loop_active) {
        g_loop_active = false;
        for (int i = 0; i < 20 && g_yield_thread_running; i++) {
            nwy_hal_os_thread_sleep(50);
        }
        if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 1000);
        MQTTDisconnect(&g_client);
        NetworkDisconnect(&g_network);
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);
        nwy_hal_os_thread_sleep(100);
    } else if (g_yield_thread_running) {
        for (int i = 0; i < 20 && g_yield_thread_running; i++) {
            nwy_hal_os_thread_sleep(50);
        }
        if (g_mqtt_mutex) nwy_sdk_mutex_lock(g_mqtt_mutex, 1000);
        NetworkDisconnect(&g_network);
        if (g_mqtt_mutex) nwy_sdk_mutex_unlock(g_mqtt_mutex);
    }
    g_yield_thread = NULL;
}
