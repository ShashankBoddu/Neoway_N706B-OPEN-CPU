#ifndef __NWY_HAL_RS485_H__
#define __NWY_HAL_RS485_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NWY_RS485_UART_PORT "URT2"
#define PIN_RS485_POWER_GPIO0 0   // Pin 83: Master Peripheral Switch Q5/Q10
#define PIN_RS485_DIR_GPIO87 87   // Pin 87: Direction Switch Q7 (High=TX, Low=RX)

/**
 * @brief Initialize RS-485 transceiver and open URT2 in direct polling mode.
 * Configures GPIO00 (Power ON) and GPIO87 (Receive mode, LOW).
 *
 * @param baudrate Serial baud rate (e.g. 9600)
 * @return true on success, false on failure
 */
bool nwy_hal_rs485_init(uint32_t baudrate);

/**
 * @brief Switch RS-485 transceiver to Transmit mode (GPIO87 = HIGH, DE=1, /RE=1)
 */
void nwy_hal_rs485_tx_mode(void);

/**
 * @brief Switch RS-485 transceiver to Receive mode (GPIO87 = LOW, DE=0, /RE=0)
 */
void nwy_hal_rs485_rx_mode(void);

/**
 * @brief Send data over the RS-485 bus in Transmit mode.
 * Sets DE/RE HIGH, transmits bytes via nwy_uart_write, and waits for
 * transmission to clear the UART shift register.
 *
 * @param data Pointer to data bytes
 * @param len Number of bytes to transmit
 * @return int Number of bytes written, or -1 on error
 */
int nwy_hal_rs485_send(const uint8_t *data, size_t len);

/**
 * @brief Direct non-blocking poll read from URT2 in polling mode
 *
 * @param buf Output buffer
 * @param max_len Maximum bytes to read
 * @return int Bytes read (>=0), or -1 on error
 */
int nwy_hal_rs485_poll_read(uint8_t *buf, size_t max_len);

/**
 * @brief Listen for incoming RS-485 data in Receive mode for a specified duration.
 * Flushes stale buffer, ensures DE/RE is LOW, and collects incoming bytes.
 *
 * @param buf Output buffer
 * @param max_len Maximum bytes to store
 * @param timeout_ms Listen duration in milliseconds (e.g. 10000 for 10 sec)
 * @return int Total bytes received
 */
int nwy_hal_rs485_listen(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/**
 * @brief De-initialize RS-485: closes URT2, sets GPIO87 LOW, cuts GPIO0 power
 */
void nwy_hal_rs485_deinit(void);

#endif /* __NWY_HAL_RS485_H__ */
