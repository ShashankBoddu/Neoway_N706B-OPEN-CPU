/**
 * @file nwy_hal_adc.c
 * @brief Industrial ADC & Battery Monitoring HAL Implementation
 */

#include "hal/nwy_hal_adc.h"
#include "hal/nwy_hal_os.h"
#include "nwy_adc_api.h"

bool nwy_hal_adc_init(void)
{
    return true;
}

int nwy_hal_adc_read_raw_mv(int samples)
{
    if (samples <= 1) {
        return nwy_adc_get(NWY_ADC_CHANNEL1, NWY_ADC_SCALE_2V444);
    }

    int sum = 0;
    int valid = 0;

    for (int i = 0; i < samples; i++) {
        int val = nwy_adc_get(NWY_ADC_CHANNEL1, NWY_ADC_SCALE_2V444);
        if (val >= 0) {
            sum += val;
            valid++;
        }
        if (i < samples - 1) {
            nwy_hal_os_thread_sleep(4);
        }
    }

    if (valid == 0) return -1;
    return (sum / valid);
}

uint8_t nwy_hal_adc_calc_percentage(uint32_t vin_mv)
{
    if (vin_mv >= 12800) return 100;
    if (vin_mv <= 10500) return 0;
    if (vin_mv >= 12400) return (uint8_t)(80 + ((vin_mv - 12400) * 20) / 400);
    if (vin_mv >= 12000) return (uint8_t)(50 + ((vin_mv - 12000) * 30) / 400);
    if (vin_mv >= 11600) return (uint8_t)(25 + ((vin_mv - 11600) * 25) / 400);
    if (vin_mv >= 11200) return (uint8_t)(10 + ((vin_mv - 11200) * 15) / 400);
    return (uint8_t)(((vin_mv - 10500) * 10) / 700);
}

bool nwy_hal_adc_get_battery(nwy_batt_status_t *out_status)
{
    if (!out_status) return false;

    int raw_mv = nwy_hal_adc_read_raw_mv(5);
    if (raw_mv < 0) {
        out_status->raw_adc_mv = 0;
        out_status->vin_mv = 0;
        out_status->percentage = 0;
        out_status->valid = false;
        return false;
    }

    out_status->raw_adc_mv = (uint32_t)raw_mv;
    /* Vin = Vadc * (12010 / 902) with integer rounding */
    out_status->vin_mv = (uint32_t)(((uint64_t)raw_mv * ADC_BATT_CALIB_VIN_MV + (ADC_BATT_CALIB_VADC_MV / 2)) / ADC_BATT_CALIB_VADC_MV);
    out_status->percentage = nwy_hal_adc_calc_percentage(out_status->vin_mv);
    out_status->valid = true;
    return true;
}
