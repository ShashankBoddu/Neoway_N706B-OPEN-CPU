#include "hal/nwy_hal_pm.h"
#include "nwy_pm_api.h"
#include <stdio.h>

void nwy_hal_pm_reboot(void) { nwy_pm_ctrl(3); }

void nwy_hal_pm_power_off(void) { nwy_pm_ctrl(2); }

bool nwy_hal_pm_get_vbat(int *vbat_mv) {
  if (!vbat_mv)
    return false;
  return (nwy_pm_vbat_voltage_get(vbat_mv) == 0);
}

bool nwy_hal_pm_set_sleep_mode(bool enable) {
  return (nwy_pm_state_set(enable ? 1 : 0) == 0);
}

bool nwy_hal_pm_sleep_wakeup_set(int wake_source, int wake_enable, int wake_level) {
  // Not exported by this SDK stub library, return false.
  return false;
}

bool nwy_hal_pm_sleep_callback_set(nwy_sleep_callback_t cb) {
  // Not exported by this SDK stub library, return false.
  return false;
}

const char *nwy_hal_pm_get_boot_reason_str(void) {
  int reason = nwy_pm_boot_res();
  if (reason & NWY_BOOTCAUSE_PSM_WAKEUP) {
    return "PSM Wakeup";
  }
  if (reason & NWY_BOOTCAUSE_PIN_WAKEUP) {
    return "GPIO Wakeup";
  }
  if (reason & NWY_BOOTCAUSE_PWRKEY) {
    return "Power Key";
  }
  if (reason & NWY_BOOTCAUSE_PIN_RESET) {
    return "Pin Reset";
  }
  if (reason & NWY_BOOTCAUSE_ALARM) {
    return "Alarm";
  }
  if (reason & NWY_BOOTCAUSE_CHARGE) {
    return "Charge In";
  }
  if (reason & NWY_BOOTCAUSE_WDG) {
    return "Watchdog";
  }
  return "Unknown";
}

bool nwy_hal_pm_get_time(nwy_time_t *time_out, int *timezone_out) {
  if (!time_out || !timezone_out)
    return false;
  return (nwy_date_get(time_out, timezone_out) == 0);
}

bool nwy_hal_pm_set_time(const nwy_time_t *time_in, int timezone) {
  if (!time_in)
    return false;
  return (nwy_date_set((nwy_time_t *)time_in, timezone) == 0);
}

bool nwy_hal_pm_date_to_timestamp(const nwy_time_t *time_in,
                                  nwy_timeval_t *timestamp_out) {
  if (!time_in || !timestamp_out)
    return false;
  return (nwy_date_to_timestamp((nwy_time_t *)time_in, timestamp_out) == 0);
}

bool nwy_hal_pm_timestamp_to_date(const nwy_timeval_t *timestamp_in,
                                  nwy_time_t *time_out) {
  if (!timestamp_in || !time_out)
    return false;
  return (nwy_timestamp_to_date((nwy_timeval_t *)timestamp_in, time_out) == 0);
}
