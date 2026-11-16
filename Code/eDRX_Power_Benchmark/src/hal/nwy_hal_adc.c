#include "hal/nwy_hal_adc.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h"
#include "nwy_adc_api.h"
#include "nwy_dev_api.h"
#include "nwy_pm_api.h"

#include <stdio.h>
#include <string.h>

#define TAG "HAL_ADC"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static bool s_adc_initialized = false;

bool nwy_hal_adc_init(void) {
  char dev_names[4][NWY_DEV_NAME_MAX_LEN];
  memset(dev_names, 0, sizeof(dev_names));

  int dev_count = nwy_dev_name_list_get(NWY_DEV_TYPE_ADC, dev_names);
  LOGI("Registered ADC Devices in Firmware (%d channels detected):", dev_count);
  for (int i = 0; i < dev_count; i++) {
    LOGI("  Channel [%d]: '%s'", i + 1, dev_names[i]);
  }

  s_adc_initialized = true;
  return true;
}

int nwy_hal_adc_read_raw(nwy_adc_e channel, nwy_adc_aux_scale_e scale) {
  int val = nwy_adc_get(channel, scale);
  if (val < 0) {
    LOGW("ADC read error on channel %d (scale %d): code %d", channel, scale, val);
  }
  return val;
}

int nwy_hal_adc_read_averaged(nwy_adc_e channel, nwy_adc_aux_scale_e scale, int samples) {
  if (samples <= 1) {
    return nwy_hal_adc_read_raw(channel, scale);
  }

  int sum = 0;
  int valid_count = 0;

  for (int i = 0; i < samples; i++) {
    int val = nwy_adc_get(channel, scale);
    if (val >= 0) {
      sum += val;
      valid_count++;
    }
    if (i < samples - 1) {
      nwy_hal_os_thread_sleep(5); // 5 ms inter-sample delay to filter transient noise
    }
  }

  if (valid_count == 0) {
    return -1;
  }

  return (sum / valid_count);
}

bool nwy_hal_adc_sample_all(nwy_hal_adc_data_t *out_data) {
  if (!out_data) {
    return false;
  }

  memset(out_data, 0, sizeof(nwy_hal_adc_data_t));

  if (!s_adc_initialized) {
    nwy_hal_adc_init();
  }

  // -------------------------------------------------------------------------
  // 1. ADC Channel 1: Module Pin 96 (Connector U5 - DC Voltage Sensor)
  // Scale: NWY_ADC_SCALE_5V000 (0 to 5.0V range to support up to 24V inputs)
  // Voltage Divider: R20=100k, R19=26.1k -> Vin = Vadc * (126.1 / 26.1)
  // -------------------------------------------------------------------------
  int adc1_val = nwy_hal_adc_read_averaged(NWY_ADC_CHANNEL1, NWY_ADC_SCALE_5V000, 5);
  if (adc1_val >= 0) {
    out_data->adc1_raw_mv = adc1_val;
    // Calculate true input voltage with integer rounding
    out_data->adc1_vin_mv = (int)(((uint64_t)adc1_val * 126100ULL + 13050ULL) / 26100ULL);
  } else {
    out_data->adc1_raw_mv = 0;
    out_data->adc1_vin_mv = 0;
  }

  // -------------------------------------------------------------------------
  // 2. ADC Channel 2: Module Pin 9 (Net INADC0 - 4-20mA Current Loop Input)
  // Scale: NWY_ADC_SCALE_2V444 (covers up to 2444 mV = 40.7 mA without clipping)
  // Shunt Resistor: R17 (120 ohm) || R18 (120 ohm) = 60.0 ohm to GND
  // Loop Current: I = Vadc / 60.0 ohm
  // -------------------------------------------------------------------------
  int adc0_val = nwy_hal_adc_read_averaged(NWY_ADC_CHANNEL2, NWY_ADC_SCALE_2V444, 5);
  if (adc0_val >= 0) {
    out_data->adc0_raw_mv = adc0_val;
    // Current in microamps: (mV * 1000) / 60
    out_data->adc0_current_ua = (int)(((uint64_t)adc0_val * 1000ULL + 30ULL) / 60ULL);
    // Current in milliamps
    out_data->adc0_current_ma = (float)adc0_val / ADC0_SHUNT_RESISTANCE_OHM;
  } else {
    out_data->adc0_raw_mv = 0;
    out_data->adc0_current_ua = 0;
    out_data->adc0_current_ma = 0.0f;
  }

  // -------------------------------------------------------------------------
  // 3. Internal VBAT Rail: Channel 4 (Battery Monitoring)
  // -------------------------------------------------------------------------
  int vbat_val = nwy_hal_adc_read_averaged(NWY_ADC_CHANNEL_VBAT, NWY_ADC_SCALE_5V000, 3);
  if (vbat_val > 0) {
    out_data->vbat_mv = vbat_val;
  } else {
    // Fallback to PMIC direct query
    int pm_vbat = 0;
    if (nwy_pm_vbat_voltage_get(&pm_vbat) == 0 && pm_vbat > 0) {
      out_data->vbat_mv = pm_vbat;
    } else {
      out_data->vbat_mv = 0;
    }
  }

  return true;
}
