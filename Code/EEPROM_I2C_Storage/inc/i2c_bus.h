#ifndef __I2C_BUS_H__
#define __I2C_BUS_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Hardware Assignments for Neoway N706B */
#define I2C2_BUS_NAME           "I2C2"
#define I2C2_DEFAULT_SCL_GPIO   10       /* Module Pin 67 (I2C2_SCL -> GPIO 10) */
#define I2C2_DEFAULT_SDA_GPIO   11       /* Module Pin 66 (I2C2_SDA -> GPIO 11) */
#define I2C_DEFAULT_SPEED_HZ    400000   /* 400 kHz Fast Mode */
#define I2C_DEFAULT_TIMEOUT_MS  50       /* 50 ms bounded transaction timeout */

/* I2C Engine Drivers */
typedef enum {
    I2C_MODE_AUTO = 0,   /* Try Hardware I2C; fallback to Bit-Bang if unresponsive */
    I2C_MODE_HW   = 1,   /* Force Hardware I2C controller */
    I2C_MODE_SW   = 2    /* Force Software Bit-Bang on GPIOs */
} i2c_driver_mode_t;

/* Bus Configuration Descriptor */
typedef struct {
    const char         *bus_name;      /* e.g., "I2C2" */
    uint32_t            speed_hz;      /* 400000 or 100000 */
    uint32_t            scl_gpio;      /* GPIO 10 */
    uint32_t            sda_gpio;      /* GPIO 11 */
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
 * @param dev_addr_7bit 7-bit slave address (e.g. 0x50 for EEPROM)
 * @return true if device responds with ACK, false otherwise.
 */
bool i2c_bus_ping(uint8_t dev_addr_7bit);

/**
 * @brief Perform a standard I2C write transaction:
 *        [START] -> [Addr + W] -> [Data 0..len-1] -> [STOP]
 * @param dev_addr_7bit 7-bit slave address
 * @param data Buffer to transmit
 * @param len Number of bytes to transmit
 * @return 0 on success (all bytes ACKed), negative error code on failure.
 */
int i2c_bus_write(uint8_t dev_addr_7bit, const uint8_t *data, uint16_t len);

/**
 * @brief Perform a standard I2C read transaction:
 *        [START] -> [Addr + R] -> [Read bytes with ACK, last with NACK] -> [STOP]
 * @param dev_addr_7bit 7-bit slave address
 * @param data Destination buffer
 * @param len Number of bytes to read
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_read(uint8_t dev_addr_7bit, uint8_t *data, uint16_t len);

/**
 * @brief Perform a combined Write-Read transaction with Repeated START:
 *        [START] -> [Addr + W] -> [tx_data] -> [Re-START] -> [Addr + R] -> [rx_data] -> [STOP]
 * @param dev_addr_7bit 7-bit slave address
 * @param tx_data Address / register data to transmit before read
 * @param tx_len Length of tx_data
 * @param rx_data Destination buffer for readback
 * @param rx_len Length of rx_data
 * @return 0 on success, negative error code on failure.
 */
int i2c_bus_write_read(uint8_t dev_addr_7bit,
                       const uint8_t *tx_data, uint16_t tx_len,
                       uint8_t *rx_data, uint16_t rx_len);

/**
 * @brief Thread-safe bus locking for multi-step composite transactions.
 * @param timeout_ms Mutex acquire timeout in ms
 * @return true if locked, false if timed out.
 */
bool i2c_bus_lock(uint32_t timeout_ms);

/**
 * @brief Release thread-safe bus lock.
 */
void i2c_bus_unlock(void);

/**
 * @brief Scan the I2C bus from address 0x08 to 0x77 and print responding devices.
 * @return Number of detected devices on the bus.
 */
int i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* __I2C_BUS_H__ */
