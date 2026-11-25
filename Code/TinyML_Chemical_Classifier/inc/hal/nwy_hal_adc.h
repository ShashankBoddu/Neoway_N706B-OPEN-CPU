/**
 * @file nwy_hal_adc.h
 * @brief Industrial ADC & Battery Monitoring HAL for Neoway N706B (ASR1605S)
 * 
 * Hardware Voltage Divider:
 *   Vin = 12.01 V -> Vadc1 = 0.902 V (902 mV)
 *   Divider Ratio K = 12010 / 902 =~ 13.314856
 *   Vin_mv = (Vadc1_mv * 12010 + 451) / 902
 */

#ifndef __NWY_HAL_ADC_H__
#define __NWY_HAL_ADC_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_adc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Calibration constants provided from bench measurement */
#define ADC_BATT_CALIB_VADC_MV   902    /* 0.902 V at ADC1 pin */
#define ADC_BATT_CALIB_VIN_MV    12010  /* 12.01 V supply input */

/**
 * @brief Battery State & Telemetry Structure
 */
typedef struct {
    uint32_t raw_adc_mv;  /* Measured pin voltage in mV (e.g. 902) */
    uint32_t vin_mv;      /* Calculated battery voltage in mV (e.g. 12010) */
    uint8_t  percentage;  /* Battery state of charge: 0 to 100 % */
    bool     valid;       /* True if ADC read succeeded */
} nwy_batt_status_t;

/**
 * @brief Initialize ADC subsystem.
 * @return true if initialized successfully.
 */
bool nwy_hal_adc_init(void);

/**
 * @brief Read raw voltage in mV from ADC1 (Pin 96) averaged over multiple samples.
 * @param samples Number of consecutive samples to average (e.g. 3 to 5).
 * @return Voltage at ADC pin in mV, or negative on error.
 */
int nwy_hal_adc_read_raw_mv(int samples);

/**
 * @brief Calculate battery percentage (0-100%) from 12V battery voltage (mV).
 * @param vin_mv Battery voltage in millivolts.
 * @return State of charge percentage (0 to 100).
 */
uint8_t nwy_hal_adc_calc_percentage(uint32_t vin_mv);

/**
 * @brief Read battery voltage and calculate percentage.
 * @param out_status Output status structure.
 * @return true on success, false on read error.
 */
bool nwy_hal_adc_get_battery(nwy_batt_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* __NWY_HAL_ADC_H__ */
