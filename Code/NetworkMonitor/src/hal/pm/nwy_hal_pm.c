#include "hal/pm/nwy_hal_pm.h"
#include "nwy_pm_api.h"
#include "nwy_log_api.h"
#include <stdio.h>

void nwy_hal_pm_reboot(void) {
    nwy_pm_ctrl(3); // 3 = reset (NWY_PM_POWER_OFF_RESET)
}

void nwy_hal_pm_power_off(void) {
    nwy_pm_ctrl(2); // 2 = sync modem then power off (NWY_PM_POWER_OFF_NORMAL)
}

bool nwy_hal_pm_get_vbat(int *vbat_mv) {
    if (!vbat_mv) return false;
    return (nwy_pm_vbat_voltage_get(vbat_mv) == 0);
}

const char *nwy_hal_pm_get_boot_reason_str(void) {
    int reason = nwy_pm_boot_res();
    switch(reason) {
        case 1: return "Power Key";
        case 2: return "Pin Reset";
        case 3: return "Alarm";
        case 4: return "Charge In";
        case 5: return "Watchdog";
        case 6: return "GPIO Wakeup";
        case 10: return "Panic Reset";
        default: return "Unknown";
    }
}
