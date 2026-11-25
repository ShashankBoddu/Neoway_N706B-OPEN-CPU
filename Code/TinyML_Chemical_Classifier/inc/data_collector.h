/**
 * @file data_collector.h
 * @brief BME688 TinyML Data Collector & SMS Command Engine Header
 */

#ifndef __DATA_COLLECTOR_H__
#define __DATA_COLLECTOR_H__

#include "bme68x.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_ALERT_PHONE "+918074638788"

typedef enum {
  COLLECTOR_STATE_IDLE = 0,
  COLLECTOR_STATE_RECORDING = 1,
  COLLECTOR_STATE_ERROR = 2
} collector_state_t;

typedef struct {
  collector_state_t state;
  bool sensor_interfaced;
  bool sd_mounted;
  char current_label[32];
  char current_file_path[64];
  char current_bme_path[128];
  uint32_t sample_count;
  uint32_t interval_sec;
  float last_temperature;
  float last_humidity;
  float last_pressure;
  float last_rgas_0;
  char last_sender_phone[32];
} collector_status_t;

/**
 * @brief Initialize data collector engine with sensor handle.
 * @param bme_dev Pointer to initialized BME688 device handle
 * @return 0 on success, negative on error.
 */
int data_collector_init(bme68x_dev_t *bme_dev);

/**
 * @brief Verify whether the BME688 sensor is successfully interfaced and ready.
 * @return true if interfaced, false if missing or communication failed.
 */
bool data_collector_is_sensor_ready(void);

/**
 * @brief Perform active runtime sensor health check and auto-recovery.
 * @return 0 if healthy/recovered, negative if offline.
 */
int data_collector_check_sensor_health(void);

/**
 * @brief Start a new dataset recording session.
 * @param label Alphanumeric class label (e.g. "TOILET_CLEAN", "TOILET_DIRTY",
 * "IPA_VAPOR")
 * @param sender_phone Optional phone number to notify
 * @param reply_buf Buffer to receive formatted status response
 * @param reply_max Max buffer size
 * @return 0 on success, negative on error.
 */
int data_collector_start(const char *label, const char *sender_phone,
                         char *reply_buf, size_t reply_max);

/**
 * @brief Stop current recording session, sync and close file on SD card.
 * @param reply_buf Buffer to receive formatted status response
 * @param reply_max Max buffer size
 * @return 0 on success, negative on error.
 */
int data_collector_stop(char *reply_buf, size_t reply_max);

/**
 * @brief Query current collector status and live environmental telemetry.
 * @param status_out Pointer to receive collector status struct
 * @param reply_buf Buffer to receive formatted status response for SMS
 * @param reply_max Max buffer size
 */
void data_collector_get_status(collector_status_t *status_out, char *reply_buf,
                               size_t reply_max);

/**
 * @brief Configure sampling interval between 10-step gas scans.
 * @param interval_sec Interval in seconds (minimum 2s)
 * @param reply_buf Buffer to receive formatted confirmation
 * @param reply_max Max buffer size
 * @return 0 on success, negative on error.
 */
int data_collector_set_interval(uint32_t interval_sec, char *reply_buf,
                                size_t reply_max);

/**
 * @brief Parse and execute a command string (shared by SMS, MQTT, and debug console).
 * @param cmd_text Command string (e.g. "START TEST1", "STOP", "STATUS", "LIST", "UPLOAD ALL", "FOTA")
 * @param sender_id Optional sender identifier (phone number for SMS, "MQTT" for MQTT, or NULL)
 * @param reply_buf Output buffer to receive response text
 * @param reply_max Maximum capacity of reply_buf
 */
void data_collector_execute_command(const char *cmd_text, const char *sender_id,
                                    char *reply_buf, size_t reply_max);

/**
 * @brief Process an incoming SMS message from operator.
 *        Parses: START <LABEL>, STOP, STATUS, INTERVAL <SEC>, HELP.
 *        Executes requested action and replies via SMS to sender.
 * @param sender_phone Phone number of the SMS sender
 * @param msg_text Body text of the SMS
 */
void data_collector_process_sms(const char *sender_phone, const char *msg_text);

/**
 * @brief Worker thread loop executing continuous periodic gas scanning when
 * recording.
 * @param param Unused
 */
void data_collector_worker_task(void *param);

/**
 * @brief Dispatch a one-time alert SMS to the configured alert recipient phone.
 * @param alert_msg Text message body
 */
void data_collector_send_alert(const char *alert_msg);

/**
 * @brief Send one-time boot status SMS upon system ready.
 * @param bme_ok Whether BME688 sensor passed self-test
 * @param sd_ok Whether MicroSD card mounted cleanly
 */
void data_collector_send_boot_status(bool bme_ok, bool sd_ok, const char *ver);

/**
 * @brief Set or update recipient phone number for proactive alerts.
 * @param phone E.164 phone string (e.g. "+918074638788")
 */
void data_collector_set_alert_phone(const char *phone);

/**
 * @brief Get currently configured alert recipient phone number.
 */
const char *data_collector_get_alert_phone(void);

#ifdef __cplusplus
}
#endif

#endif /* __DATA_COLLECTOR_H__ */
