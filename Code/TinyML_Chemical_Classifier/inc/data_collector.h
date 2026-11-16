/**
 * @file data_collector.h
 * @brief BME688 TinyML Data Collector & SMS Command Engine Header
 */

#ifndef __DATA_COLLECTOR_H__
#define __DATA_COLLECTOR_H__

#include <stdint.h>
#include <stdbool.h>
#include "bme68x.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COLLECTOR_STATE_IDLE = 0,
    COLLECTOR_STATE_RECORDING = 1,
    COLLECTOR_STATE_ERROR = 2
} collector_state_t;

typedef struct {
    collector_state_t state;
    char              current_label[32];
    char              current_file_path[64];
    uint32_t          sample_count;
    uint32_t          interval_sec;
    float             last_temperature;
    float             last_humidity;
    float             last_pressure;
    float             last_rgas_0;
    char              last_sender_phone[32];
} collector_status_t;

/**
 * @brief Initialize data collector engine with sensor handle.
 * @param bme_dev Pointer to initialized BME688 device handle
 * @return 0 on success, negative on error.
 */
int data_collector_init(bme68x_dev_t *bme_dev);

/**
 * @brief Start a new dataset recording session.
 * @param label Alphanumeric class label (e.g. "TOILET_CLEAN", "TOILET_DIRTY", "IPA_VAPOR")
 * @param sender_phone Optional phone number to notify
 * @param reply_buf Buffer to receive formatted status response
 * @param reply_max Max buffer size
 * @return 0 on success, negative on error.
 */
int data_collector_start(const char *label, const char *sender_phone, char *reply_buf, size_t reply_max);

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
void data_collector_get_status(collector_status_t *status_out, char *reply_buf, size_t reply_max);

/**
 * @brief Configure sampling interval between 10-step gas scans.
 * @param interval_sec Interval in seconds (minimum 2s)
 * @param reply_buf Buffer to receive formatted confirmation
 * @param reply_max Max buffer size
 * @return 0 on success, negative on error.
 */
int data_collector_set_interval(uint32_t interval_sec, char *reply_buf, size_t reply_max);

/**
 * @brief Process an incoming SMS message from operator.
 *        Parses: START <LABEL>, STOP, STATUS, INTERVAL <SEC>, HELP.
 *        Executes requested action and replies via SMS to sender.
 * @param sender_phone Phone number of the SMS sender
 * @param msg_text Body text of the SMS
 */
void data_collector_process_sms(const char *sender_phone, const char *msg_text);

/**
 * @brief Worker thread loop executing continuous periodic gas scanning when recording.
 * @param param Unused
 */
void data_collector_worker_task(void *param);

#ifdef __cplusplus
}
#endif

#endif /* __DATA_COLLECTOR_H__ */
