/**
 * @file i2c_bus.h
 * @brief Industrial I2C1 Bus Subsystem for Neoway N706B OpenCPU
 * 
 * Configured for external sensor connection on I2C1 (Module Pin 57 SCL / GPIO 49,
 * Module Pin 58 SDA / GPIO 50) level-shifted via U8 to VBATI2C1_SCL and VBATI2C1_SDA.
 */

#ifndef __I2C_BUS_H__
#define __I2C_BUS_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Hardware Assignments for External I2C Bus 1 */
#define I2C1_BUS_NAME           "I2C1"
#define I2C1_DEFAULT_SCL_GPIO   49       /* Module Pin 57 (I2C1_SCL -> GPIO 49) */
#define I2C1_DEFAULT_SDA_GPIO   50       /* Module Pin 58 (I2C1_SDA -> GPIO 50) */
#define I2C_DEFAULT_SPEED_HZ    100000   /* 100 kHz Standard Mode (Rock-solid for level shifters & sensors) */
#define I2C_DEFAULT_TIMEOUT_MS  50       /* 50 ms bounded transaction timeout */

/* I2C Engine Drivers */
typedef enum {
    I2C_MODE_AUTO = 0,   /* Try Hardware I2C; fallback to Bit-Bang if unresponsive */
    I2C_MODE_HW   = 1,   /* Force Hardware I2C controller */
    I2C_MODE_SW   = 2    /* Force Software Bit-Bang on GPIOs */
} i2c_driver_mode_t;

/* Bus Configuration Descriptor */
typedef struct {
    const char         *bus_name;      /* e.g., "I2C1" */
    uint32_t            speed_hz;      /* 400000 or 100000 */
    uint32_t            scl_gpio;      /* GPIO 49 */
    uint32_t            sda_gpio;      /* GPIO 50 */
    i2c_driver_mode_t   mode;          /* I2C_MODE_AUTO, I2C_MODE_HW, or I2C_MODE_SW */
    uint32_t            timeout_ms;    /* Transaction timeout in ms */
} i2c_bus_config_t;

/**
 * @brief Thread-safe formatted logging to debug UART.
 */
void i2c_log(const char *fmt, ...);

/**
 * @brief Execute standard 9-clock bus unstick sequence to release stuck slave SDA.
 * @param scl_gpio GPIO number for SCL line
 * @param sda_gpio GPIO number for SDA line
 * @return 0 if bus is clean and free, negative if SDA remains stuck low.
 */
int i2c_bus_recover(uint32_t scl_gpio, uint32_t sda_gpio);

/**
 * @brief Initialize I2C bus subsystem (mutex, recovery, controller/bitbang setup).
 * @param cfg Pointer to bus configuration (or NULL for defaults).
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_init(const i2c_bus_config_t *cfg);

/**
 * @brief Deinitialize I2C bus subsystem and release resources.
 */
void i2c_bus_deinit(void);

/**
 * @brief Probe if a device at 7-bit address responds with an ACK.
 * @param dev_addr_7bit 7-bit slave address (e.g. 0x76 for BME688)
 * @return true if device responds with ACK, false otherwise.
 */
bool i2c_bus_ping(uint8_t dev_addr_7bit);

/**
 * @brief Perform a standard I2C write transaction.
 */
int i2c_bus_write(uint8_t dev_addr_7bit, const uint8_t *data, uint16_t len);

/**
 * @brief Perform a standard I2C read transaction.
 */
int i2c_bus_read(uint8_t dev_addr_7bit, uint8_t *data, uint16_t len);

/**
 * @brief Perform a combined Write-Read transaction with Repeated START.
 */
int i2c_bus_write_read(uint8_t dev_addr_7bit,
                       const uint8_t *tx_data, uint16_t tx_len,
                       uint8_t *rx_data, uint16_t rx_len);

/**
 * @brief Reset and reinitialize the I2C bus subsystem (useful for bus hang recovery).
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_reset(void);

/**
 * @brief Read consecutive registers starting at reg_addr from a 7-bit slave device.
 * @param dev_addr_7bit 7-bit slave address (e.g. 0x76 for BME688)
 * @param reg_addr Starting 8-bit register address (e.g. 0xD0 for Chip ID)
 * @param data Buffer to receive data read from registers
 * @param len Number of bytes to read
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_read_reg(uint8_t dev_addr_7bit, uint8_t reg_addr, uint8_t *data, uint16_t len);

/**
 * @brief Write consecutive registers starting at reg_addr to a 7-bit slave device.
 * @param dev_addr_7bit 7-bit slave address (e.g. 0x76 for BME688)
 * @param reg_addr Starting 8-bit register address
 * @param data Buffer containing payload data to write
 * @param len Number of payload bytes to write
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_write_reg(uint8_t dev_addr_7bit, uint8_t reg_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Recursive mutex lock/unlock for thread safety across multiple FreeRTOS tasks.
 */
bool i2c_bus_lock(uint32_t timeout_ms);
void i2c_bus_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* __I2C_BUS_H__ */
