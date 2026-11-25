#ifndef __MQTT_MANAGER_H__
#define __MQTT_MANAGER_H__

#include <stdbool.h>
#include <stdint.h>

#define MQTT_BROKER_HOST    "mqtt.fervidlabs.in"
#define MQTT_BROKER_PORT    1883
#define MQTT_USER           "fervid"
#define MQTT_PASS           "Fervid@123"
#define MQTT_CLIENT_ID      "N706B_BME688_01"

/* Topics requested by user */
#define MQTT_PUB_TOPIC      "sensor/input/N706B/BM688"
#define MQTT_SUB_TOPIC      "sensor/output/N706B/BM688"

/**
 * @brief Initialize MQTT manager background task and auto-reconnect engine.
 */
bool mqtt_manager_init(void);

/**
 * @brief Publish a plain text message to MQTT_PUB_TOPIC.
 * @param msg Null-terminated string.
 * @return true if published, false if offline or failed.
 */
bool mqtt_manager_publish(const char *msg);

/**
 * @brief Check if MQTT client is currently connected and active.
 */
bool mqtt_manager_is_connected(void);

#endif /* __MQTT_MANAGER_H__ */
