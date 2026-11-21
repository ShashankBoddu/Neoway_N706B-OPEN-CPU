#ifndef __NWY_HAL_SECURE_FOTA_H__
#define __NWY_HAL_SECURE_FOTA_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOTA_STATE_IDLE = 0,
    FOTA_STATE_DOWNLOADING,
    FOTA_STATE_VERIFYING,
    FOTA_STATE_FLASHING,
    FOTA_STATE_SUCCESS,
    FOTA_STATE_FAILED
} fota_state_e;

/**
 * @brief Initiate secure FOTA update from specified URL with SHA-256 + ECDSA signature verification.
 * @param url Full HTTP/HTTPS URL of the nwy_open_app_fota.pkt binary.
 * @param file_size Exact file size in bytes.
 * @param signature_hex Hex string of the ECDSA secp256r1 signature (DER format).
 * @return true if download initiated, false if busy or invalid params.
 */
bool nwy_hal_secure_fota_start(const char *url, uint32_t file_size,
                               const char *signature_hex);

/**
 * @brief Initiate secure FOTA with an SMS notification callback to the given phone number.
 * @param url Full HTTP/HTTPS URL of the firmware package.
 * @param file_size Exact file size in bytes.
 * @param signature_hex Hex string of ECDSA signature.
 * @param notify_phone Operator phone number to send progress/completion SMS alerts to.
 * @return true if initiated, false otherwise.
 */
bool nwy_hal_secure_fota_start_with_notify(const char *url, uint32_t file_size,
                                           const char *signature_hex,
                                           const char *notify_phone);

/**
 * @brief Check current FOTA update state and progress.
 * @param downloaded_bytes Output pointer for received bytes.
 * @param total_bytes Output pointer for expected total bytes.
 * @return Current FOTA state enum.
 */
fota_state_e nwy_hal_secure_fota_get_status(uint32_t *downloaded_bytes, uint32_t *total_bytes);

/**
 * @brief Cancel an active FOTA download in progress.
 */
void nwy_hal_secure_fota_cancel(void);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SECURE_FOTA_H__
