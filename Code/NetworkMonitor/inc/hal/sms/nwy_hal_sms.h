#ifndef __NWY_HAL_SMS_H__
#define __NWY_HAL_SMS_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback definition for receiving an SMS
 * @param phone_num The sender's phone number
 * @param message The SMS text content
 * @param timestamp The timestamp of the message (YYYY-MM-DD HH:MM:SS)
 */
typedef void (*nwy_hal_sms_recv_callback_t)(const char *phone_num, const char *message, const char *timestamp);

/**
 * @brief Initialize the SMS module for a given SIM slot
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @return true if successful
 */
bool nwy_hal_sms_init(int sim_id);

/**
 * @brief Register a callback for incoming SMS
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param cb The callback function to invoke
 * @return true if successful
 */
bool nwy_hal_sms_register_recv_cb(int sim_id, nwy_hal_sms_recv_callback_t cb);

/**
 * @brief Send an SMS to a specific phone number
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param phone_num Destination phone number
 * @param message Message body (ASCII/GSM7 text)
 * @return true if successful
 */
bool nwy_hal_sms_send(int sim_id, const char *phone_num, const char *message);

/**
 * @brief Delete an SMS at a specific memory index
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param index The index to delete
 * @return true if successful
 */
bool nwy_hal_sms_delete(int sim_id, int index);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SMS_H__
