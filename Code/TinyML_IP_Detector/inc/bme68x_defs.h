/**
 * @file bme68x_defs.h
 * @brief Bosch BME688 / BME680 Environmental Gas Scanner Sensor Definitions
 * 
 * Hardware-level register addresses, bit masks, calibration structures,
 * and heater profile configurations for the Bosch BME688 AI Gas Sensor.
 */

#ifndef __BME68X_DEFS_H__
#define __BME68X_DEFS_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sensor Hardware Constants */
#define BME68X_CHIP_ID                  UINT8_C(0x61)
#define BME68X_I2C_ADDR_LOW             UINT8_C(0x76)
#define BME68X_I2C_ADDR_HIGH            UINT8_C(0x77)

/* Register Addresses */
#define BME68X_REG_COEFF3               UINT8_C(0x00)
#define BME68X_REG_COEFF2               UINT8_C(0x8A)
#define BME68X_REG_COEFF1               UINT8_C(0xE1)
#define BME68X_REG_CHIP_ID              UINT8_C(0xD0)
#define BME68X_REG_RESET                UINT8_C(0xE0)
#define BME68X_REG_VARIANT_ID           UINT8_C(0xF0)

#define BME68X_REG_IDAC_HEAT_0          UINT8_C(0x50)
#define BME68X_REG_RES_HEAT_0           UINT8_C(0x5A)
#define BME68X_REG_GAS_WAIT_0           UINT8_C(0x64)
#define BME68X_REG_GAS_WAIT_SHARED      UINT8_C(0x6E)
#define BME68X_REG_CTRL_GAS_0           UINT8_C(0x70)
#define BME68X_REG_CTRL_GAS_1           UINT8_C(0x71)
#define BME68X_REG_CTRL_HUM             UINT8_C(0x72)
#define BME68X_REG_CTRL_MEAS            UINT8_C(0x74)
#define BME68X_REG_CONFIG               UINT8_C(0x75)
#define BME68X_REG_MEM_PAGE             UINT8_C(0x73)

/* Field Data Registers (Field 0) */
#define BME68X_REG_MEAS_STATUS_0        UINT8_C(0x1D)
#define BME68X_REG_PRESS_MSB_0          UINT8_C(0x1F)
#define BME68X_REG_PRESS_LSB_0          UINT8_C(0x20)
#define BME68X_REG_PRESS_XLSB_0         UINT8_C(0x21)
#define BME68X_REG_TEMP_MSB_0           UINT8_C(0x22)
#define BME68X_REG_TEMP_LSB_0           UINT8_C(0x23)
#define BME68X_REG_TEMP_XLSB_0          UINT8_C(0x24)
#define BME68X_REG_HUM_MSB_0            UINT8_C(0x25)
#define BME68X_REG_HUM_LSB_0            UINT8_C(0x26)
#define BME68X_REG_GAS_R_MSB_0          UINT8_C(0x2C)
#define BME68X_REG_GAS_R_LSB_0          UINT8_C(0x2D)

/* Soft Reset Command */
#define BME68X_CMD_SOFT_RESET           UINT8_C(0xB6)

/* Operating Modes */
#define BME68X_SLEEP_MODE               UINT8_C(0x00)
#define BME68X_FORCED_MODE              UINT8_C(0x01)
#define BME68X_PARALLEL_MODE            UINT8_C(0x02)
#define BME68X_SEQUENTIAL_MODE          UINT8_C(0x03)

/* Oversampling Settings */
#define BME68X_OS_NONE                  UINT8_C(0)
#define BME68X_OS_1X                    UINT8_C(1)
#define BME68X_OS_2X                    UINT8_C(2)
#define BME68X_OS_4X                    UINT8_C(3)
#define BME68X_OS_8X                    UINT8_C(4)
#define BME68X_OS_16X                   UINT8_C(5)

/* IIR Filter Settings */
#define BME68X_FILTER_OFF               UINT8_C(0)
#define BME68X_FILTER_SIZE_1            UINT8_C(1)
#define BME68X_FILTER_SIZE_3            UINT8_C(2)
#define BME68X_FILTER_SIZE_7            UINT8_C(3)
#define BME68X_FILTER_SIZE_15           UINT8_C(4)
#define BME68X_FILTER_SIZE_31           UINT8_C(5)
#define BME68X_FILTER_SIZE_63           UINT8_C(6)
#define BME68X_FILTER_SIZE_127          UINT8_C(7)

/* Heater Enable Flags */
#define BME68X_ENABLE_HEATER            UINT8_C(0x00)
#define BME68X_DISABLE_HEATER           UINT8_C(0x08)
#define BME68X_ENABLE_GAS_MEAS          UINT8_C(0x20)
#define BME68X_DISABLE_GAS_MEAS         UINT8_C(0x00)

/* Status Bits */
#define BME68X_NEW_DATA_BIT             UINT8_C(0x80)
#define BME68X_GAS_MEASURING_BIT        UINT8_C(0x40)
#define BME68X_MEASURING_BIT            UINT8_C(0x20)
#define BME68X_GAS_VALID_BIT            UINT8_C(0x20)
#define BME68X_HEAT_STAB_BIT            UINT8_C(0x10)

/* Return Status Codes */
#define BME68X_OK                       INT8_C(0)
#define BME68X_E_NULL_PTR               INT8_C(-1)
#define BME68X_E_COM_FAIL               INT8_C(-2)
#define BME68X_E_DEV_NOT_FOUND          INT8_C(-3)
#define BME68X_E_INVALID_LENGTH         INT8_C(-4)
#define BME68X_W_NO_NEW_DATA            INT8_C(1)

/* Sensor Variants */
#define BME68X_VARIANT_GAS_LOW          UINT8_C(0x00)
#define BME68X_VARIANT_GAS_HIGH         UINT8_C(0x01)

/* Standard HP-354 Multi-Heater Profile (10 Steps for TinyML Gas Scanning) */
#define BME68X_HP_354_STEPS             10

/**
 * @brief Bosch BME688 Calibration Coefficients structure
 */
typedef struct {
    uint16_t par_t1;
    int16_t  par_t2;
    int8_t   par_t3;
    uint16_t par_p1;
    int16_t  par_p2;
    int8_t   par_p3;
    int16_t  par_p4;
    int16_t  par_p5;
    int8_t   par_p6;
    int8_t   par_p7;
    int16_t  par_p8;
    int16_t  par_p9;
    uint8_t  par_p10;
    uint16_t par_h1;
    uint16_t par_h2;
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;
    int8_t   par_gh1;
    int16_t  par_gh2;
    int8_t   par_gh3;
    uint8_t  res_heat_range;
    int8_t   res_heat_val;
    int8_t   range_sw_err;
    float    t_fine;
} bme68x_calib_data_t;

/**
 * @brief Environmental and Gas measurement sample structure
 */
typedef struct {
    float    temperature;    /* Temperature in degrees Celsius */
    float    pressure;       /* Barometric Pressure in hPa */
    float    humidity;       /* Relative Humidity in % RH */
    float    gas_resistance; /* Gas resistance in Ohms */
    uint8_t  status;         /* Status byte (gas valid, heat stab) */
    uint8_t  gas_index;      /* Heater profile step index (0-9) */
    uint8_t  meas_index;     /* Measurement cycle counter */
} bme68x_data_t;

/**
 * @brief Heater step configuration descriptor
 */
typedef struct {
    uint16_t temp_prof[10];  /* Heater temperatures in °C (e.g. 200 to 400) */
    uint16_t dur_prof[10];   /* Dwell duration in ms (e.g. 140ms) */
    uint8_t  profile_len;    /* Number of steps (up to 10) */
} bme68x_heatr_conf_t;

/**
 * @brief Environmental sensor oversampling and filter configuration
 */
typedef struct {
    uint8_t os_hum;
    uint8_t os_temp;
    uint8_t os_pres;
    uint8_t filter;
    uint8_t odr;
} bme68x_conf_t;

#ifdef __cplusplus
}
#endif

#endif /* __BME68X_DEFS_H__ */
