/**
 * @file bme68x.c
 * @brief Bosch BME688 AI Gas Scanner Sensor Implementation
 */

#include "bme68x.h"
#include <string.h>

/* Lookup tables for low gas resistance range compensation */
static const float s_lookup_k1_range[16] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -0.8f,
    0.0f, 0.0f, -0.2f, -0.5f, 0.0f, -1.0f, 0.0f, 0.0f
};

static const float s_lookup_k2_range[16] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.7f, 0.0f, -0.8f,
    -0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

/* Official Bosch HP-354 Multi-Heater Gas Scanning Profile
 * Steps: 320°C (700ms), 100°C (280ms), 100°C (1400ms), 100°C (4032ms),
 *        200°C (700ms), 200°C (700ms), 200°C (700ms),
 *        320°C (700ms), 320°C (700ms), 320°C (700ms)
 * Total duration: 10,612 ms (~10.8s) per scan */
static const uint16_t s_hp354_temp_steps[10] = {320, 100, 100, 100, 200, 200, 200, 320, 320, 320};
static const uint16_t s_hp354_dur_steps[10]  = {700, 280, 1400, 4032, 700, 700, 700, 700, 700, 700};

/* =========================================================================
 * INTERNAL MATH & COMPENSATION FUNCTIONS
 * ========================================================================= */

static uint8_t calc_res_heat(uint16_t temp, const bme68x_dev_t *dev)
{
    if (temp > 400) temp = 400;

    int32_t var1 = (((int32_t)dev->amb_temp * dev->calib.par_gh3) / 1000) * 256;
    int32_t var2 = (dev->calib.par_gh1 + 784) *
                   (((((dev->calib.par_gh2 + 154009) * temp * 5) / 100) + 3276800) / 10);
    int32_t var3 = var1 + (var2 / 2);
    int32_t var4 = (var3 / (dev->calib.res_heat_range + 4));
    int32_t var5 = (131 * dev->calib.res_heat_val) + 65536;
    int32_t heatr_res_x100 = (int32_t)(((var4 / var5) - 250) * 34);

    return (uint8_t)((heatr_res_x100 + 50) / 100);
}

static uint8_t calc_gas_wait(uint16_t dur_ms)
{
    uint8_t factor = 0;
    uint8_t durval;

    if (dur_ms >= 0x0FC0) {
        durval = 0xFF;
    } else {
        while (dur_ms > 0x3F) {
            dur_ms /= 4;
            factor += 1;
        }
        durval = (uint8_t)(dur_ms + (factor * 64));
    }
    return durval;
}

static float calc_temperature(uint32_t temp_adc, bme68x_dev_t *dev)
{
    float var1 = (((float)temp_adc / 16384.0f) - ((float)dev->calib.par_t1 / 1024.0f)) * ((float)dev->calib.par_t2);
    float var2 = ((((float)temp_adc / 131072.0f) - ((float)dev->calib.par_t1 / 8192.0f)) *
                  (((float)temp_adc / 131072.0f) - ((float)dev->calib.par_t1 / 8192.0f))) *
                 ((float)dev->calib.par_t3 * 16.0f);
    dev->calib.t_fine = var1 + var2;
    return dev->calib.t_fine / 5120.0f;
}

static float calc_pressure(uint32_t pres_adc, const bme68x_dev_t *dev)
{
    float var1 = ((dev->calib.t_fine / 2.0f) - 64000.0f);
    float var2 = var1 * var1 * (((float)dev->calib.par_p6) / (131072.0f));
    var2 = var2 + (var1 * ((float)dev->calib.par_p5) * 2.0f);
    var2 = (var2 / 4.0f) + (((float)dev->calib.par_p4) * 65536.0f);
    var1 = (((((float)dev->calib.par_p3 * var1 * var1) / 16384.0f) +
             ((float)dev->calib.par_p2 * var1)) / 524288.0f);
    var1 = (1.0f + (var1 / 32768.0f)) * ((float)dev->calib.par_p1);

    float calc_pres = 0.0f;
    if (var1 > 0.0f || var1 < 0.0f) {
        calc_pres = 1048576.0f - (float)pres_adc;
        calc_pres = (((calc_pres - (var2 / 4096.0f)) * 6250.0f) / var1);
        var1 = (((float)dev->calib.par_p9) * calc_pres * calc_pres) / 2147483648.0f;
        var2 = calc_pres * (((float)dev->calib.par_p8) / 32768.0f);
        float var3 = (calc_pres / 256.0f) * (calc_pres / 256.0f) * (calc_pres / 256.0f) *
                     ((float)dev->calib.par_p10 / 131072.0f);
        calc_pres = (calc_pres + (var1 + var2 + var3 + ((float)dev->calib.par_p7 * 128.0f)) / 16.0f) / 100.0f; /* Convert to hPa */
    }
    return calc_pres;
}

static float calc_humidity(uint16_t hum_adc, const bme68x_dev_t *dev)
{
    float temp_comp = dev->calib.t_fine / 5120.0f;
    float var1 = (float)hum_adc - (((float)dev->calib.par_h1 * 16.0f) +
                 (((float)dev->calib.par_h3 / 2.0f) * temp_comp));
    float var2 = var1 * (((float)dev->calib.par_h2 / 262144.0f) *
                 (1.0f + (((float)dev->calib.par_h4 / 16384.0f) * temp_comp) +
                  (((float)dev->calib.par_h5 / 1048576.0f) * temp_comp * temp_comp)));
    float var3 = (float)dev->calib.par_h6 / 16384.0f;
    float var4 = (float)dev->calib.par_h7 / 2097152.0f;
    float calc_hum = var2 + ((var3 + (var4 * temp_comp)) * var2 * var2);

    if (calc_hum > 100.0f) calc_hum = 100.0f;
    else if (calc_hum < 0.0f) calc_hum = 0.0f;

    return calc_hum;
}

static float calc_gas_resistance_low(uint16_t gas_res_adc, uint8_t gas_range, const bme68x_dev_t *dev)
{
    if (gas_range > 15) return 0.0f;

    float gas_res_f = (float)gas_res_adc;
    float gas_range_f = (float)(1 << gas_range);
    float var1 = 1340.0f + (5.0f * (float)dev->calib.range_sw_err);
    float var2 = var1 * (1.0f + s_lookup_k1_range[gas_range] / 100.0f);
    float var3 = 1.0f + (s_lookup_k2_range[gas_range] / 100.0f);
    float denominator = var3 * 0.000000125f * gas_range_f * (((gas_res_f - 512.0f) / var2) + 1.0f);

    if (denominator > 0.0f) {
        return 1.0f / denominator;
    }
    return 0.0f;
}

static float calc_gas_resistance_high(uint16_t gas_res_adc, uint8_t gas_range)
{
    if (gas_range > 15) return 0.0f;

    uint32_t var1 = UINT32_C(262144) >> gas_range;
    int32_t var2 = (int32_t)gas_res_adc - INT32_C(512);
    var2 *= INT32_C(3);
    var2 = INT32_C(4096) + var2;

    if (var2 > 0) {
        return (1000000.0f * (float)var1) / (float)var2;
    }
    return 0.0f;
}

/* =========================================================================
 * CALIBRATION COEFFICIENTS READOUT
 * ========================================================================= */
static int8_t read_calibration_data(bme68x_dev_t *dev)
{
    uint8_t coeff1[16];
    uint8_t coeff2[24];
    uint8_t coeff3[8];

    /* Read block 1: 0xE1 to 0xF0 */
    if (dev->read(BME68X_REG_COEFF1, coeff1, sizeof(coeff1), dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    /* Read block 2: 0x8A to 0xA1 */
    if (dev->read(BME68X_REG_COEFF2, coeff2, sizeof(coeff2), dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    /* Read block 3: 0x00 to 0x04 */
    if (dev->read(BME68X_REG_COEFF3, coeff3, sizeof(coeff3), dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    /* Temperature Trim */
    dev->calib.par_t1 = (uint16_t)(((uint16_t)coeff1[9] << 8) | coeff1[8]);
    dev->calib.par_t2 = (int16_t)(((uint16_t)coeff2[1] << 8) | coeff2[0]);
    dev->calib.par_t3 = (int8_t)coeff2[2];

    /* Pressure Trim */
    dev->calib.par_p1 = (uint16_t)(((uint16_t)coeff2[5] << 8) | coeff2[4]);
    dev->calib.par_p2 = (int16_t)(((uint16_t)coeff2[7] << 8) | coeff2[6]);
    dev->calib.par_p3 = (int8_t)coeff2[8];
    dev->calib.par_p4 = (int16_t)(((uint16_t)coeff2[11] << 8) | coeff2[10]);
    dev->calib.par_p5 = (int16_t)(((uint16_t)coeff2[13] << 8) | coeff2[12]);
    dev->calib.par_p6 = (int8_t)coeff2[15];
    dev->calib.par_p7 = (int8_t)coeff2[14];
    dev->calib.par_p8 = (int16_t)(((uint16_t)coeff2[19] << 8) | coeff2[18]);
    dev->calib.par_p9 = (int16_t)(((uint16_t)coeff2[21] << 8) | coeff2[20]);
    dev->calib.par_p10 = (uint8_t)coeff2[22];

    /* Humidity Trim */
    dev->calib.par_h1 = (uint16_t)(((uint16_t)coeff1[2] << 4) | (coeff1[1] & 0x0F));
    dev->calib.par_h2 = (uint16_t)(((uint16_t)coeff1[0] << 4) | ((coeff1[1] >> 4) & 0x0F));
    dev->calib.par_h3 = (int8_t)coeff1[3];
    dev->calib.par_h4 = (int8_t)coeff1[4];
    dev->calib.par_h5 = (int8_t)coeff1[5];
    dev->calib.par_h6 = (uint8_t)coeff1[6];
    dev->calib.par_h7 = (int8_t)coeff1[7];

    /* Gas Trim */
    dev->calib.par_gh1 = (int8_t)coeff1[12];
    dev->calib.par_gh2 = (int16_t)(((uint16_t)coeff1[11] << 8) | coeff1[10]);
    dev->calib.par_gh3 = (int8_t)coeff1[13];

    /* Heat Range & Value */
    dev->calib.res_heat_val = (int8_t)coeff3[0];
    dev->calib.res_heat_range = (uint8_t)((coeff3[2] >> 4) & 0x03);
    dev->calib.range_sw_err = (int8_t)((int8_t)coeff3[4] >> 4);

    return BME68X_OK;
}

/* =========================================================================
 * PUBLIC API IMPLEMENTATION
 * ========================================================================= */

int8_t bme68x_init(bme68x_dev_t *dev)
{
    if (dev == NULL || dev->read == NULL || dev->write == NULL || dev->delay_us == NULL) {
        return BME68X_E_NULL_PTR;
    }

    /* 1. Issue Soft Reset */
    uint8_t reset_cmd = BME68X_CMD_SOFT_RESET;
    dev->write(BME68X_REG_RESET, &reset_cmd, 1, dev->intf_ptr);
    dev->delay_us(25000, dev->intf_ptr); /* 25 ms reset settling */

    /* 2. Read & Validate Chip ID (0x61) with retries */
    uint8_t chip_id = 0;
    int8_t rslt = BME68X_E_COM_FAIL;
    for (int retry = 0; retry < 5; retry++) {
        if (dev->read(BME68X_REG_CHIP_ID, &chip_id, 1, dev->intf_ptr) == 0) {
            if (chip_id == BME68X_CHIP_ID) {
                rslt = BME68X_OK;
                break;
            }
        }
        dev->delay_us(10000, dev->intf_ptr);
    }

    dev->chip_id = chip_id;
    if (rslt != BME68X_OK) {
        if (chip_id != 0 && chip_id != BME68X_CHIP_ID) {
            return BME68X_E_DEV_NOT_FOUND;
        }
        return BME68X_E_COM_FAIL;
    }

    /* 3. Read Variant ID (BME688 returns 0x01 / BME680 returns 0x00) */
    uint8_t variant_id = 0;
    dev->read(BME68X_REG_VARIANT_ID, &variant_id, 1, dev->intf_ptr);
    dev->variant_id = variant_id;

    /* 4. Load Factory Calibration Trimming Parameters */
    if (read_calibration_data(dev) != BME68X_OK) {
        return BME68X_E_COM_FAIL;
    }

    if (dev->amb_temp <= 0.0f) {
        dev->amb_temp = 25.0f;
    }

    return BME68X_OK;
}

int8_t bme68x_set_conf(const bme68x_conf_t *conf, bme68x_dev_t *dev)
{
    if (conf == NULL || dev == NULL) return BME68X_E_NULL_PTR;

    /* Humidity oversampling: ctrl_hum (0x72) */
    uint8_t ctrl_hum = conf->os_hum & 0x07;
    if (dev->write(BME68X_REG_CTRL_HUM, &ctrl_hum, 1, dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    /* Temperature & Pressure oversampling: ctrl_meas (0x74) */
    uint8_t ctrl_meas = ((conf->os_temp & 0x07) << 5) | ((conf->os_pres & 0x07) << 2);
    if (dev->write(BME68X_REG_CTRL_MEAS, &ctrl_meas, 1, dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    /* IIR filter setting: config (0x75) */
    uint8_t config = (conf->filter & 0x07) << 2;
    if (dev->write(BME68X_REG_CONFIG, &config, 1, dev->intf_ptr) != 0) return BME68X_E_COM_FAIL;

    return BME68X_OK;
}

int8_t bme68x_set_heatr_conf(uint8_t op_mode, const bme68x_heatr_conf_t *conf, bme68x_dev_t *dev)
{
    if (conf == NULL || dev == NULL) return BME68X_E_NULL_PTR;
    (void)op_mode;

    uint8_t profile_len = conf->profile_len;
    if (profile_len > 10) profile_len = 10;

    for (uint8_t i = 0; i < profile_len; i++) {
        uint8_t res_heat = calc_res_heat(conf->temp_prof[i], dev);
        uint8_t gas_wait = calc_gas_wait(conf->dur_prof[i]);

        dev->write(BME68X_REG_RES_HEAT_0 + i, &res_heat, 1, dev->intf_ptr);
        dev->write(BME68X_REG_GAS_WAIT_0 + i, &gas_wait, 1, dev->intf_ptr);
    }

    /* Enable Gas Measurements: ctrl_gas_0 (0x70) = 0x00, ctrl_gas_1 (0x71) = 0x20 | (nb_conv) */
    uint8_t ctrl_gas_0 = BME68X_ENABLE_HEATER;
    dev->write(BME68X_REG_CTRL_GAS_0, &ctrl_gas_0, 1, dev->intf_ptr);

    uint8_t ctrl_gas_1 = BME68X_ENABLE_GAS_MEAS | ((profile_len - 1) & 0x0F);
    dev->write(BME68X_REG_CTRL_GAS_1, &ctrl_gas_1, 1, dev->intf_ptr);

    return BME68X_OK;
}

int8_t bme68x_set_op_mode(uint8_t op_mode, bme68x_dev_t *dev)
{
    if (dev == NULL) return BME68X_E_NULL_PTR;

    uint8_t ctrl_meas = 0;
    dev->read(BME68X_REG_CTRL_MEAS, &ctrl_meas, 1, dev->intf_ptr);

    ctrl_meas = (ctrl_meas & 0xFC) | (op_mode & 0x03);
    if (dev->write(BME68X_REG_CTRL_MEAS, &ctrl_meas, 1, dev->intf_ptr) != 0) {
        return BME68X_E_COM_FAIL;
    }

    return BME68X_OK;
}

bool bme68x_check_connection(bme68x_dev_t *dev)
{
    if (dev == NULL || dev->read == NULL) return false;
    uint8_t chip_id = 0;
    int8_t rslt = dev->read(BME68X_REG_CHIP_ID, &chip_id, 1, dev->intf_ptr);
    return (rslt == BME68X_OK && chip_id == BME68X_CHIP_ID);
}

int8_t bme68x_get_data(uint8_t op_mode, bme68x_data_t *data, uint8_t *n_data, bme68x_dev_t *dev)
{
    if (data == NULL || n_data == NULL || dev == NULL) return BME68X_E_NULL_PTR;
    (void)op_mode;

    uint8_t buff[17];
    *n_data = 0;

    /* Read field 0 data block: 0x1D to 0x2D with bounded retry for conversion ready */
    bool ready = false;
    for (int retry = 0; retry < 5; retry++) {
        if (dev->read(BME68X_REG_MEAS_STATUS_0, buff, sizeof(buff), dev->intf_ptr) != 0) {
            return BME68X_E_COM_FAIL;
        }
        if (buff[0] & BME68X_NEW_DATA_BIT) {
            ready = true;
            break;
        }
        dev->delay_us(10000, dev->intf_ptr); /* wait 10 ms for ADC completion */
    }

    if (!ready) {
        return BME68X_W_NO_NEW_DATA;
    }

    /* Extract Pressure ADC (20-bit) */
    uint32_t pres_adc = ((uint32_t)buff[2] << 12) | ((uint32_t)buff[3] << 4) | ((uint32_t)buff[4] >> 4);

    /* Extract Temperature ADC (20-bit) */
    uint32_t temp_adc = ((uint32_t)buff[5] << 12) | ((uint32_t)buff[6] << 4) | ((uint32_t)buff[7] >> 4);

    /* Extract Humidity ADC (16-bit) */
    uint16_t hum_adc = ((uint16_t)buff[8] << 8) | buff[9];

    /* Extract Gas ADC (10-bit) and Range (4-bit) */
    uint16_t gas_res_adc = ((uint16_t)buff[15] << 2) | ((uint16_t)buff[16] >> 6);
    uint8_t gas_range = buff[16] & 0x0F;
    uint8_t gas_status = buff[16] & 0x30; /* Valid bit 5, heat_stab bit 4 */

    /* Perform floating-point compensations */
    data->temperature = calc_temperature(temp_adc, dev);
    data->pressure = calc_pressure(pres_adc, dev);
    data->humidity = calc_humidity(hum_adc, dev);

    /* Update running ambient temperature */
    dev->amb_temp = data->temperature;

    /* Compute gas resistance */
    if (dev->variant_id == BME68X_VARIANT_GAS_HIGH) {
        data->gas_resistance = calc_gas_resistance_high(gas_res_adc, gas_range);
    } else {
        data->gas_resistance = calc_gas_resistance_low(gas_res_adc, gas_range, dev);
    }

    data->status = gas_status;
    data->gas_index = buff[0] & 0x0F;
    *n_data = 1;

    return BME68X_OK;
}

int8_t bme68x_perform_10step_scan(bme68x_dev_t *dev, float r_gas_out[10],
                                 float *temp, float *hum, float *pres)
{
    if (dev == NULL || r_gas_out == NULL) return BME68X_E_NULL_PTR;

    /* Fast presence pre-check */
    if (!bme68x_check_connection(dev)) {
        return BME68X_E_DEV_NOT_FOUND;
    }

    /* Ensure oversampling defaults: Temp 2x, Pres 4x, Hum 1x, Filter Size 3 */
    bme68x_conf_t conf;
    conf.os_temp = BME68X_OS_2X;
    conf.os_pres = BME68X_OS_4X;
    conf.os_hum  = BME68X_OS_1X;
    conf.filter  = BME68X_FILTER_SIZE_3;
    conf.odr     = 0;
    if (bme68x_set_conf(&conf, dev) != BME68X_OK) {
        return BME68X_E_COM_FAIL;
    }

    /* Sequential execution through all 10 HP-354 steps */
    for (uint8_t step = 0; step < 10; step++) {
        uint8_t res_heat = calc_res_heat(s_hp354_temp_steps[step], dev);
        uint8_t gas_wait = calc_gas_wait(s_hp354_dur_steps[step]);

        /* Set step 0 register */
        if (dev->write(BME68X_REG_RES_HEAT_0, &res_heat, 1, dev->intf_ptr) != BME68X_OK ||
            dev->write(BME68X_REG_GAS_WAIT_0, &gas_wait, 1, dev->intf_ptr) != BME68X_OK) {
            return BME68X_E_COM_FAIL;
        }

        /* Arm gas heater */
        uint8_t ctrl_gas_0 = BME68X_ENABLE_HEATER;
        dev->write(BME68X_REG_CTRL_GAS_0, &ctrl_gas_0, 1, dev->intf_ptr);

        uint8_t ctrl_gas_1 = BME68X_ENABLE_GAS_MEAS | 0x00; /* Step index 0 */
        dev->write(BME68X_REG_CTRL_GAS_1, &ctrl_gas_1, 1, dev->intf_ptr);

        /* Trigger Forced Mode */
        if (bme68x_set_op_mode(BME68X_FORCED_MODE, dev) != BME68X_OK) {
            return BME68X_E_COM_FAIL;
        }

        /* Dwell delay: heater duration + 10ms ADC settling time (optimized for fast cycle rate) */
        uint32_t dwell_us = ((uint32_t)s_hp354_dur_steps[step] + 10) * 1000;
        dev->delay_us(dwell_us, dev->intf_ptr);

        /* Read back field 0 */
        bme68x_data_t sample;
        uint8_t n_data = 0;
        int8_t rslt = bme68x_get_data(BME68X_FORCED_MODE, &sample, &n_data, dev);

        if (rslt == BME68X_OK && n_data > 0) {
            r_gas_out[step] = sample.gas_resistance;
            if (step == 0) {
                if (temp) *temp = sample.temperature;
                if (hum)  *hum  = sample.humidity;
                if (pres) *pres = sample.pressure;
            }
        } else {
            /* Strict industrial-grade failure: Do NOT fake or corrupt dataset! */
            return BME68X_E_COM_FAIL;
        }
    }

    return BME68X_OK;
}
