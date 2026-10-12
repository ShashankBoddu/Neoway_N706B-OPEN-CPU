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
 * @brief Flush and clear UART internal RX buffers and reset capture state.
 */
void nwy_hal_rs485_flush_rx(void);

/**
 * @brief De-initialize RS-485: closes URT2, sets GPIO87 LOW, cuts GPIO0 power
 */
void nwy_hal_rs485_deinit(void);

/* =========================================================================
 * MODBUS RTU & HYDROSTATIC LEVEL SENSOR INTERFACE
 * ========================================================================= */

typedef enum {
  MODBUS_STATUS_NOT_READ      = 0U,
  MODBUS_STATUS_OK            = 1U,
  MODBUS_STATUS_NO_RESPONSE   = 2U,
  MODBUS_STATUS_CRC_ERROR     = 3U,
  MODBUS_STATUS_FRAME_ERROR   = 4U,
  MODBUS_STATUS_INVALID_VALUE = 5U
} ModbusStatusCode;

typedef struct {
  bool             is_valid;       /* True if level measurement was successfully parsed */
  float            level_mm;       /* Water level in millimeters (mm) */
  float            level_m;        /* Water level in meters (m) */
  uint16_t         raw_register;   /* Raw 16-bit register value read from sensor */
  ModbusStatusCode status;         /* Modbus transaction status code */
} nwy_hydro_sensor_data_t;

/**
 * @brief Calculate Modbus RTU 16-bit CRC (Polynomial: 0xA001, Init: 0xFFFF)
 * @param data Pointer to input data buffer
 * @param len Length of data in bytes
 * @return uint16_t Computed CRC-16
 */
uint16_t nwy_hal_modbus_calc_crc(const uint8_t *data, uint16_t len);

/**
 * @brief Execute a half-duplex Modbus RTU send/receive transaction over RS-485.
 *
 * Automatically wraps command in slave address and CRC16, asserts TX mode (DE=1),
 * transmits frame, automatically reverts to RX mode (DE=0), and validates response CRC.
 *
 * @param address Modbus slave device address (e.g. 0x01)
 * @param command Pointer to PDU command bytes (e.g. {0x03, 0x00, 0x04, 0x00, 0x01})
 * @param command_length Length of command bytes
 * @param response Buffer to store received raw response
 * @param expected_response_length Expected total response length (including addr and CRC)
 * @param timeout_ms Maximum time in milliseconds to wait for full response
 * @return ModbusStatusCode Result of the transaction
 */
ModbusStatusCode nwy_hal_modbus_send_receive(
    uint8_t address,
    const uint8_t *command,
    uint8_t command_length,
    uint8_t *response,
    uint8_t expected_response_length,
    uint32_t timeout_ms);

/**
 * @brief Power on, stabilize, read, and power off the RS-485 Hydrostatic Level Sensor.
 *
 * Follows exact industrial procedure:
 * 1. Enables VCC_SWout switched rail (GPIO00=1) to power transducer and transceiver at 4800 baud.
 * 2. Waits 3000 ms for analog transducer oscillator stabilization.
 * 3. Transmits Modbus RTU Read Holding Register (Addr 0x01, Func 0x03, Reg 0x0004, Count 0x0001).
 * 4. Parses level data in millimeters and converts to meters.
 * 5. Immediately cuts power to transceiver and sensor (GPIO00=0) to preserve battery.
 *
 * @param sensor_out Pointer to output structure receiving parsed water level and status
 * @return bool true if valid level reading was obtained, false otherwise
 */
bool nwy_hal_rs485_read_hydro_sensor(nwy_hydro_sensor_data_t *sensor_out);

#endif /* __NWY_HAL_RS485_H__ */
