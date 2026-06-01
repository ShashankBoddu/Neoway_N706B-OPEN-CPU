#ifndef __NWY_HAL_SNTP_H__
#define __NWY_HAL_SNTP_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SNTP callback signature
 * @param success true if SNTP synchronization was successful, false otherwise
 */
typedef void (*nwy_hal_sntp_sync_cb_t)(bool success);

/**
 * @brief Synchronize the system time using SNTP (NTP protocol)
 * @param cid The data channel ID (usually 1 if PDP profile 1 is connected)
 * @param ntp_server The NTP server URL (e.g., "pool.ntp.org")
 * @param tz Timezone string (e.g., "E8" for UTC+8, "E5" for UTC+5, etc.)
 * @param cb Callback function when sync completes
 * @return true if the sync request was sent successfully
 */
bool nwy_hal_sntp_sync_time(int cid, const char* ntp_server, const char* tz, nwy_hal_sntp_sync_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SNTP_H__
