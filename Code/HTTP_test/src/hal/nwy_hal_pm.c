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

const char *nwy_hal_pm_get_boot_reason_str(void) {
  int reason = nwy_pm_boot_res();
  switch (reason) {
  case 1:
    return "Power Key";
  case 2:
    return "Pin Reset";
  case 3:
    return "Alarm";
  case 4:
    return "Charge In";
  case 5:
    return "Watchdog";
  case 6:
    return "GPIO Wakeup";
  case 10:
    return "Panic Reset";
  default:
    return "Unknown";
  }
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
