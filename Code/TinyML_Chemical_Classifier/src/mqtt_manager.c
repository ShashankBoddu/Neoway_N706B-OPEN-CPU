#include "mqtt_manager.h"
#include "data_collector.h"
#include "hal/nwy_hal_mqtt.h"
#include "hal/net/nwy_hal_net.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/uart/nwy_hal_uart.h"
#include "nwy_osi_api.h"
#include <stdio.h>
#include <string.h>

#define TAG "MQTT_MGR"

#define MQTT_CMD_Q_CAPACITY 8
#define MQTT_CMD_MAX_LEN    384
#define MQTT_TOPIC_MAX_LEN  128

typedef struct {
    char topic[MQTT_TOPIC_MAX_LEN];
    char cmd[MQTT_CMD_MAX_LEN];
} mqtt_cmd_item_t;

static mqtt_cmd_item_t  g_cmd_queue[MQTT_CMD_Q_CAPACITY];
static int              g_cmd_q_head = 0;
static int              g_cmd_q_tail = 0;
static nwy_osi_mutex_t     g_cmd_q_mutex = NULL;
static nwy_osi_semaphore_t g_cmd_q_sem = NULL;

static nwy_osi_thread_t g_mqtt_mgr_thread = NULL;
static nwy_osi_thread_t g_cmd_worker_thread = NULL;
static volatile bool    g_mqtt_mgr_running = false;

/* -------------------------------------------------------------------------
 * FAST INCOMING PAHO CALLBACK (Called from Paho yield pump)
 * Pushes command to queue and returns immediately to keep yield pump unblocked.
 * ------------------------------------------------------------------------- */
static void on_mqtt_incoming_message(const char *topic, const uint8_t *payload, uint32_t len)
{
    if (!payload || len == 0) return;

    if (g_cmd_q_mutex) nwy_sdk_mutex_lock(g_cmd_q_mutex, 100);
    int next_head = (g_cmd_q_head + 1) % MQTT_CMD_Q_CAPACITY;
    if (next_head != g_cmd_q_tail) {
        if (topic) {
            strncpy(g_cmd_queue[g_cmd_q_head].topic, topic, MQTT_TOPIC_MAX_LEN - 1);
            g_cmd_queue[g_cmd_q_head].topic[MQTT_TOPIC_MAX_LEN - 1] = '\0';
        } else {
            g_cmd_queue[g_cmd_q_head].topic[0] = '\0';
        }

        uint32_t cpy_len = len;
        if (cpy_len >= MQTT_CMD_MAX_LEN) cpy_len = MQTT_CMD_MAX_LEN - 1;
        memcpy(g_cmd_queue[g_cmd_q_head].cmd, payload, cpy_len);
        g_cmd_queue[g_cmd_q_head].cmd[cpy_len] = '\0';

        g_cmd_q_head = next_head;
    } else {
        nwy_hal_uart_log_w(TAG, "MQTT command queue full! Dropping packet.");
    }
    if (g_cmd_q_mutex) nwy_sdk_mutex_unlock(g_cmd_q_mutex);

    if (g_cmd_q_sem) {
        nwy_hal_os_semaphore_release(g_cmd_q_sem);
    }
}

/* -------------------------------------------------------------------------
 * DECOUPLED ASYNCHRONOUS COMMAND WORKER THREAD (Industrial Standard)
 * Executes commands outside of MQTTYield context so responses never lock the socket.
 * ------------------------------------------------------------------------- */
static void mqtt_cmd_worker_task(void *param)
{
    (void)param;
    nwy_hal_uart_log_i(TAG, "MQTT command dispatcher thread active.");

    while (g_mqtt_mgr_running) {
        /* Block until a command arrives in the queue */
        nwy_hal_os_semaphore_acquire(g_cmd_q_sem, 0xFFFFFFFF);

        mqtt_cmd_item_t item;
        bool has_item = false;

        if (g_cmd_q_mutex) nwy_sdk_mutex_lock(g_cmd_q_mutex, 100);
        if (g_cmd_q_head != g_cmd_q_tail) {
            item = g_cmd_queue[g_cmd_q_tail];
            g_cmd_q_tail = (g_cmd_q_tail + 1) % MQTT_CMD_Q_CAPACITY;
            has_item = true;
        }
        if (g_cmd_q_mutex) nwy_sdk_mutex_unlock(g_cmd_q_mutex);

        if (has_item) {
            nwy_hal_uart_printf(1, "\r\n======================================================\r\n");
            nwy_hal_uart_printf(1, ">>> INCOMING MQTT COMMAND RECEIVED <<<\r\n");
            nwy_hal_uart_printf(1, "  * Topic      : %s\r\n", item.topic[0] ? item.topic : "N/A");
            nwy_hal_uart_printf(1, "  * Command    : \"%s\"\r\n", item.cmd);
            nwy_hal_uart_printf(1, "======================================================\r\n");

            /* Execute unified command logic (START, STOP, STATUS, LIST, UPLOAD, DELETE, SERVER, FOTA) */
            char reply[1024];
            memset(reply, 0, sizeof(reply));
            data_collector_execute_command(item.cmd, "MQTT", reply, sizeof(reply));

            /* Transmit response back to MQTT publish topic */
            if (reply[0] != '\0') {
                nwy_hal_uart_printf(1, "[MQTT TX] Publishing response to %s: \"%s\"\r\n", MQTT_PUB_TOPIC, reply);
                mqtt_manager_publish(reply);
            }
        }
    }
    nwy_thread_exit(NULL);
}

/* -------------------------------------------------------------------------
 * MQTT CONNECTION & HEALTH MONITOR THREAD
 * Handles automatic connection, topic subscription, and auto-reconnect.
 * ------------------------------------------------------------------------- */
static void mqtt_manager_worker_task(void *param)
{
    (void)param;
    nwy_hal_uart_log_i(TAG, "MQTT Connection Manager worker started.");

    /* Initial grace period for cellular LTE data call (CID 1) to stabilize */
    nwy_hal_os_thread_sleep(4000);

    while (g_mqtt_mgr_running) {
        char local_ip[32] = {0};
        bool net_ok = nwy_hal_net_get_ip(1, 1, local_ip, sizeof(local_ip));

        if (net_ok && strlen(local_ip) > 0 && strcmp(local_ip, "0.0.0.0") != 0) {
            if (!nwy_hal_mqtt_is_connected()) {
                nwy_hal_uart_log_i(TAG, "Connecting to broker %s:%d (IP: %s)...",
                                   MQTT_BROKER_HOST, MQTT_BROKER_PORT, local_ip);

                if (nwy_hal_mqtt_init(1, MQTT_BROKER_HOST, MQTT_BROKER_PORT,
                                      MQTT_USER, MQTT_PASS, on_mqtt_incoming_message)) {
                    nwy_hal_uart_log_i(TAG, "Connected! Subscribing to: %s", MQTT_SUB_TOPIC);
                    bool sub_ok = nwy_hal_mqtt_subscribe(MQTT_SUB_TOPIC, 0, on_mqtt_incoming_message);
                    if (sub_ok) {
                        nwy_hal_uart_log_i(TAG, "Subscribed successfully to %s", MQTT_SUB_TOPIC);
                    } else {
                        nwy_hal_uart_log_e(TAG, "Failed to subscribe to %s", MQTT_SUB_TOPIC);
                    }

                    // Announce online presence on MQTT_PUB_TOPIC
                    mqtt_manager_publish("NEOWAY N706B BME688 ONLINE (MQTT Dual-Channel Active)");
                } else {
                    nwy_hal_uart_log_w(TAG, "MQTT connection attempt failed. Retrying in 5s...");
                }
            }
        } else {
            nwy_hal_uart_log_w(TAG, "Cellular IP not assigned yet. Waiting...");
        }

        nwy_hal_os_thread_sleep(5000);
    }

    nwy_hal_mqtt_close();
    nwy_thread_exit(NULL);
}

bool mqtt_manager_init(void)
{
    if (g_mqtt_mgr_running) return true;

    g_cmd_q_head = 0;
    g_cmd_q_tail = 0;
    if (!g_cmd_q_mutex) nwy_sdk_mutex_create(&g_cmd_q_mutex);
    if (!g_cmd_q_sem) nwy_hal_os_semaphore_create(&g_cmd_q_sem, 0, MQTT_CMD_Q_CAPACITY);

    g_mqtt_mgr_running = true;

    /* 1. Asynchronous Command Processor Task (Priority 44, 8KB Stack) */
    nwy_thread_create(&g_cmd_worker_thread, "mqtt_cmd_wrk", 44,
                      mqtt_cmd_worker_task, NULL, 10, 1024 * 8, NULL);

    /* 2. Connection Health Manager Task (Priority 45, 8KB Stack) */
    nwy_thread_create(&g_mqtt_mgr_thread, "mqtt_conn_mgr", 45,
                      mqtt_manager_worker_task, NULL, 10, 1024 * 8, NULL);

    return true;
}

bool mqtt_manager_publish(const char *msg)
{
    if (!msg || !nwy_hal_mqtt_is_connected()) {
        return false;
    }
    return nwy_hal_mqtt_publish(MQTT_PUB_TOPIC, msg, 0, false);
}

bool mqtt_manager_is_connected(void)
{
    return nwy_hal_mqtt_is_connected();
}
