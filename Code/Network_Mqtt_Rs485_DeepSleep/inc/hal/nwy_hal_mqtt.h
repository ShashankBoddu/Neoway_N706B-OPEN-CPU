#ifndef __NWY_HAL_MQTT_H__
#define __NWY_HAL_MQTT_H__

#include <stdbool.h>
#include <stdint.h>

// Callback signature for processing incoming messages
typedef void (*nwy_hal_mqtt_cb_t)(const char *topic, const uint8_t *payload,
                                  uint32_t len);

bool nwy_hal_mqtt_init(uint16_t cid, const char *host, uint16_t port,
                       const char *user, const char *pass,
                       nwy_hal_mqtt_cb_t cb);

bool nwy_hal_mqtt_subscribe(const char *topic, int qos, nwy_hal_mqtt_cb_t cb);

bool nwy_hal_mqtt_publish(const char *topic, const char *payload, int qos,
                          bool retain);

void nwy_hal_mqtt_close(void);

#endif