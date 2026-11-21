#ifndef __NWY_HAL_SNTP_H__
#define __NWY_HAL_SNTP_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SNTP callback signature triggered upon synchronization loop
 * termination.
 * @param success true if network sync succeeded and local clock is aligned,
 * false otherwise.
 */
typedef void (*nwy_hal_sntp_sync_cb_t)(bool success);

/**
 * @brief Synchronizes the system time using premium server cascades over
 * cellular UDP.
 * @param cid The data channel connection identifier index (1 to 6).
 * @param ntp_server Primary NTP server URL string (e.g., "pool.ntp.org").
 * @param tz Timezone string formatted as per specification (e.g., "E5").
 * @param cb User application callback event hook to receive final status
 * notification.
 * @return true if the core tracking engine initiated the sync pipeline safely,
 * false on invalid parameters.
 */
bool nwy_hal_sntp_sync_time(int cid, const char *ntp_server, const char *tz,
                            nwy_hal_sntp_sync_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SNTP_H__