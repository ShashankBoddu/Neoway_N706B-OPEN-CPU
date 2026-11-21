#ifndef IP_DETECTOR_H
#define IP_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_ALERT_PHONE         "+918074638788"
#define IP_CONFIRM_CYCLES           2        /* 2 confirmed consecutive cycles with multi-tier validation */
#define IP_CONFIDENCE_THRESHOLD     0.95f    /* 95.0% confidence threshold (Real IP is 99.7%; breath is 86.3%) */

/* Chemical Physical Verification Thresholds for Pure Isopropanol */
#define IP_MAX_HUMIDITY             56.0f    /* Real IP is 30-45% RH; Mouth breath is >65-95% RH */
#define IP_MAX_LOW_TEMP_GAS_OHMS    120000.0f/* Real IP S1-S3 is 20-58k; Mouth breath is 660-840k */
#define IP_MAX_HOT_TEMP_GAS_OHMS    20000.0f /* Real IP S0,S7-S9 is ~5.7k; Mouth breath is 38-70k */

typedef enum {
    DETECTOR_STATE_IDLE = 0,
    DETECTOR_STATE_MONITORING,
    DETECTOR_STATE_CONFIRMING,
    DETECTOR_STATE_ALERT_LATCHED,
    DETECTOR_STATE_INTERFERENCE_REJECTED
} detector_state_t;

typedef struct {
    detector_state_t state;
    uint32_t         cycle_count;
    uint32_t         consecutive_ip_cycles;
    float            last_ip_confidence;
    float            last_normal_confidence;
    float            last_temp;
    float            last_pres;
    float            last_hum;
    bool             sms_alert_sent;
    uint32_t         sms_alert_timestamp;
    char             alert_phone[32];
} detector_status_t;

#include "bme68x.h"

/**
 * @brief Initialize BME688 gas scanner and TinyML inference engine.
 * @param bme_dev Pointer to initialized BME688 device structure.
 * @return true on success.
 */
bool ip_detector_init(bme68x_dev_t *bme_dev);

/**
 * @brief Start continuous background detection task.
 * @return true on success.
 */
bool ip_detector_start(void);

/**
 * @brief Stop background detection task.
 */
void ip_detector_stop(void);

/**
 * @brief Get current live detector status.
 */
void ip_detector_get_status(detector_status_t *status);

/**
 * @brief Reset SMS alert latch to allow a new SMS alert.
 */
void ip_detector_reset_alert(void);

/**
 * @brief Handle incoming SMS paging commands (STATUS, RESET).
 */
void ip_detector_handle_sms_command(const char *sender_phone, const char *msg_body);

#ifdef __cplusplus
}
#endif

#endif /* IP_DETECTOR_H */
