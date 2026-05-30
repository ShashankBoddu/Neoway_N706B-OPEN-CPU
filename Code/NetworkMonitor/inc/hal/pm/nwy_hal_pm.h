#ifndef __NWY_HAL_PM_H__
#define __NWY_HAL_PM_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_osi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Power states controls
void nwy_hal_pm_reboot(void);
void nwy_hal_pm_power_off(void);
bool nwy_hal_pm_get_vbat(int *vbat_mv);
const char *nwy_hal_pm_get_boot_reason_str(void);

// Calendar & Time APIs
bool nwy_hal_pm_get_time(nwy_time_t *time_out, int *timezone_out);
bool nwy_hal_pm_set_time(const nwy_time_t *time_in, int timezone);
bool nwy_hal_pm_date_to_timestamp(const nwy_time_t *time_in, nwy_timeval_t *timestamp_out);
bool nwy_hal_pm_timestamp_to_date(const nwy_timeval_t *timestamp_in, nwy_time_t *time_out);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_PM_H__
