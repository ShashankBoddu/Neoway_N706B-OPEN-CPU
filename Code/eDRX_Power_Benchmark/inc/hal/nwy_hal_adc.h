#ifndef __NWY_HAL_ADC_H__
#define __NWY_HAL_ADC_H__

#include <stdbool.h>
#include <stdint.h>
#include "nwy_adc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// HARDWARE SCHEMATIC CALIBRATION & VOLTAGE DIVIDER RATIOS
// Hardware Platform: ELEKTRONIKA_N706B_prototype (ASR1605S Cat-1)
// =============================================================================

// ADC1 (Module Pin 96, Connector U5 - External DC Voltage Input):
// Divider: R20 (100k ohm series) and R19 (26.1k ohm pull-down to GND), PESD3V3T1BLYL TVS
// Vin = Vadc * (R20 + R19) / R19 = Vadc * 126.1 / 26.1 =~ Vadc * 4.8314176
#define ADC1_R_HIGH_OHM          100000.0f
#define ADC1_R_LOW_OHM           26100.0f
#define ADC1_DIVIDER_RATIO       ((ADC1_R_HIGH_OHM + ADC1_R_LOW_OHM) / ADC1_R_LOW_OHM) // ~4.8314176

// ADC0 (Module Pin 9, Net INADC0 - 4-20mA Industrial Current Loop Input):
// Shunt: R17 (120 ohm) in parallel with R18 (120 ohm) = 60.0 ohm shunt to GND, PESD3V3T1BLYL TVS
// I_current = Vadc / 60.0 ohm
// At 4 mA:  Vadc = 4mA  * 60 ohm = 240 mV
// At 20 mA: Vadc = 20mA * 60 ohm = 1200 mV (fits within 1.25V / 2.44V scale)
#define ADC0_SHUNT_RESISTANCE_OHM 60.0f

typedef struct {
  // ADC Channel 1 (Pin 96, Connector U5 - External Voltage)
  int      adc1_raw_mv;          // Measured pin voltage in mV (0 - 5000 mV)
  int      adc1_vin_mv;          // Calculated true input voltage at U5 Pin 1 (0 - 24000 mV)

  // ADC Channel 2 (Pin 9, Net INADC0 / ADC0 - 4-20mA Current Loop)
  int      adc0_raw_mv;          // Measured pin voltage across 60 ohm shunt in mV
  int      adc0_current_ua;      // Calculated loop current in microamps (uA)
  float    adc0_current_ma;      // Calculated loop current in milliamps (mA)

  // Internal Battery Rail
  int      vbat_mv;              // Internal PMIC / VBAT rail voltage in mV
} nwy_hal_adc_data_t;

/**
 * @brief Initialize the ADC HAL driver and query registered hardware channels.
 * @return true if initialized successfully.
 */
bool nwy_hal_adc_init(void);

/**
 * @brief Read raw voltage in millivolts from a specific hardware ADC channel.
 * @param channel NWY_ADC_CHANNEL1 (Pin 96), NWY_ADC_CHANNEL2 (Pin 9), or NWY_ADC_CHANNEL_VBAT
 * @param scale Selected full-scale range (NWY_ADC_SCALE_1V250, 2V444, 3V233, 5V000)
 * @return Acquired voltage in millivolts (>= 0), or negative error code (< 0)
 */
int nwy_hal_adc_read_raw(nwy_adc_e channel, nwy_adc_aux_scale_e scale);

/**
 * @brief Multi-sample averaging read on an ADC channel to eliminate electrical noise.
 * @param channel Target ADC channel
 * @param scale Target ADC scale
 * @param samples Number of consecutive samples to average (e.g. 5)
 * @return Filtered voltage in millivolts, or negative error code
 */
int nwy_hal_adc_read_averaged(nwy_adc_e channel, nwy_adc_aux_scale_e scale, int samples);

/**
 * @brief Sample all board sensors (ADC1 Vin, ADC0 4-20mA loop, VBAT) with engineering conversions.
 * @param out_data Pointer to output structure
 * @return true if readings were captured successfully
 */
bool nwy_hal_adc_sample_all(nwy_hal_adc_data_t *out_data);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_ADC_H__
