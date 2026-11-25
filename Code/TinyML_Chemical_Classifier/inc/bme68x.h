/**
 * @file bme68x.h
 * @brief High-level Bosch BME688 AI Gas Scanner Driver Header
 */

#ifndef __BME68X_H__
#define __BME68X_H__

#include "bme68x_defs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t (*bme68x_read_fptr_t)(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr);
typedef int8_t (*bme68x_write_fptr_t)(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr);
typedef void   (*bme68x_delay_us_fptr_t)(uint32_t period_us, void *intf_ptr);

/**
 * @brief BME688 Device Handle
 */
typedef struct {
    uint8_t                 chip_id;
    uint8_t                 dev_addr;
    uint8_t                 variant_id;
    float                   amb_temp;       /* Ambient temperature for heater calculations (°C) */
    bme68x_calib_data_t     calib;          /* Calibration coefficients */
    bme68x_read_fptr_t      read;           /* I2C read callback */
    bme68x_write_fptr_t     write;          /* I2C write callback */
    bme68x_delay_us_fptr_t  delay_us;       /* Microsecond sleep callback */
    void                   *intf_ptr;       /* Optional interface context pointer */
} bme68x_dev_t;

/**
 * @brief Initialize BME688 sensor (soft-reset, chip ID verification, calibration readout).
 * @param dev Pointer to device descriptor
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_init(bme68x_dev_t *dev);

/**
 * @brief Fast, non-intrusive runtime health check of BME688 sensor.
 *        Probes register 0xD0 (Chip ID) and checks for 0x61.
 * @param dev Pointer to device handle
 * @return true if sensor is online and healthy, false if disconnected/unresponsive.
 */
bool bme68x_check_connection(bme68x_dev_t *dev);

/**
 * @brief Configure sensor oversampling and digital IIR filter.
 * @param conf Pointer to configuration struct
 * @param dev Pointer to device descriptor
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_set_conf(const bme68x_conf_t *conf, bme68x_dev_t *dev);

/**
 * @brief Configure multi-heater gas scanning profiles.
 * @param op_mode Operating mode (BME68X_FORCED_MODE or BME68X_PARALLEL_MODE)
 * @param conf Pointer to heater configuration
 * @param dev Pointer to device descriptor
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_set_heatr_conf(uint8_t op_mode, const bme68x_heatr_conf_t *conf, bme68x_dev_t *dev);

/**
 * @brief Set sensor operational mode (Sleep, Forced, Parallel).
 * @param op_mode Target mode
 * @param dev Pointer to device descriptor
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_set_op_mode(uint8_t op_mode, bme68x_dev_t *dev);

/**
 * @brief Read compensated measurement fields from sensor.
 * @param op_mode Current operating mode
 * @param data Array to receive field data
 * @param n_data Pointer returning number of valid fields read
 * @param dev Pointer to device descriptor
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_get_data(uint8_t op_mode, bme68x_data_t *data, uint8_t *n_data, bme68x_dev_t *dev);

/**
 * @brief High-reliability 10-step sequential Gas Scanner routine (HP-354 standard profile).
 *        Cycles the internal micro-hotplate through 10 distinct temperature setpoints:
 *        [200°C, 240°C, 280°C, 300°C, 320°C, 340°C, 360°C, 380°C, 400°C, 200°C]
 *        and records the 10-point chemical fingerprint.
 * @param dev Pointer to device descriptor
 * @param r_gas_out 10-element float array to store R0..R9 (in Ohms)
 * @param temp Output pointer for ambient temperature (°C)
 * @param hum Output pointer for relative humidity (% RH)
 * @param pres Output pointer for barometric pressure (hPa)
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_perform_10step_scan(bme68x_dev_t *dev, float r_gas_out[10],
                                 float *temp, float *hum, float *pres);

/**
 * @brief Fast non-blocking environmental read (T, H, P) in forced mode (~15ms).
 *        No micro-hotplate heater delay.
 * @param dev Pointer to device descriptor
 * @param temp Output pointer for ambient temperature (°C)
 * @param hum Output pointer for relative humidity (% RH)
 * @param pres Output pointer for barometric pressure (hPa)
 * @return BME68X_OK on success, negative error code on failure.
 */
int8_t bme68x_read_basic(bme68x_dev_t *dev, float *temp, float *hum, float *pres);

#ifdef __cplusplus
}
#endif

#endif /* __BME68X_H__ */
