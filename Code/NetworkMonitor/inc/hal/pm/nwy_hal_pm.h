#ifndef __NWY_HAL_PM_H__
#define __NWY_HAL_PM_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reboot the module
 */
void nwy_hal_pm_reboot(void);

/**
 * @brief Power off the module normally (syncs modem before shutting down)
 */
void nwy_hal_pm_power_off(void);

/**
 * @brief Get the battery voltage
 * @param vbat_mv Pointer to store the voltage in millivolts
 * @return true if successful
 */
bool nwy_hal_pm_get_vbat(int *vbat_mv);

/**
 * @brief Get the boot reason as a static string
 * @return String description of the boot reason
 */
const char *nwy_hal_pm_get_boot_reason_str(void);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_PM_H__
