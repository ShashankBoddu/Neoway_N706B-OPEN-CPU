#include "hal/nwy_hal_rs485.h"
#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h"
#include "nwy_gpio_api.h"
#include "nwy_uart_api.h"

#include <stdio.h>
#include <string.h>

#define TAG "HAL_RS485"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

#define NWY_CORE_UART_MODE_ADDR  0x7e22e3c0
#define NWY_CORE_LOG_CONFIG_ADDR 0x7e212578

static int g_rs485_fd = -1;
static uint32_t g_rs485_baudrate = 9600;

void nwy_hal_rs485_tx_mode(void) {
  // GPIO87 controls Q7 level shifter to RS_De (RE#/DE):
  // HIGH (1.8V): Q7 OFF -> RS_De pulled HIGH to VBAT (DE=1, RE#=1: Transmit Mode)
  nwy_hal_gpio_set_value(PIN_RS485_DIR_GPIO87, true);
}

void nwy_hal_rs485_rx_mode(void) {
  // LOW (0.0V): Q7 ON -> RS_De pulled LOW to GND (DE=0, RE#=0: Receive Mode)
  nwy_hal_gpio_set_value(PIN_RS485_DIR_GPIO87, false);
}

void nwy_hal_rs485_flush_rx(void) {
  // Drain any pending bytes from hardware UART FIFO / driver queue
  if (g_rs485_fd >= 0) {
    uint8_t trash[64];
    int n;
    int safety_limit = 50;
    do {
      memset(trash, 0, sizeof(trash));
      n = nwy_uart_read(g_rs485_fd, trash, sizeof(trash) - 1);
    } while (n > 0 && --safety_limit > 0);
  }
  // Transient settling delay
  nwy_hal_os_thread_sleep(10);
}

bool nwy_hal_rs485_init(uint32_t baudrate) {
  g_rs485_baudrate = (baudrate > 0) ? baudrate : 9600;
  LOGI("Initializing RS-485 on %s at %lu baud (Pure Polling Architecture)...",
       NWY_RS485_UART_PORT, (unsigned long)g_rs485_baudrate);

  // 1. Power ON peripheral switch Q5/Q10 (GPIO0 = 1)
  nwy_hal_gpio_init_out(PIN_RS485_POWER_GPIO0, true);
  nwy_hal_gpio_set_value(PIN_RS485_POWER_GPIO0, true);
  LOGI("Transceiver VCC Rail (GPIO00): POWERED (ON)");

  // 2. Configure Direction Switch Q7 (GPIO87 = 0 -> Receive Mode)
  nwy_hal_gpio_init_out(PIN_RS485_DIR_GPIO87, false);
  nwy_hal_rs485_rx_mode();

  // Allow hardware power rails and level shifters to stabilize
  nwy_hal_os_thread_sleep(50);

  // 3. Open UART port (URT2, Pins 17/18 MAIN_TXD/MAIN_RXD)
  if (g_rs485_fd >= 0) {
    nwy_hal_os_thread_sleep(10);
    nwy_uart_close(g_rs485_fd);
    g_rs485_fd = -1;
  }

  // Critical Crane OS Configuration:
  // 1. Enforce global uartMode = NWY_UART_MODE_DATA (1) at address 0x7e22e3c0.
  //    This ensures nwy_uart_open initializes driver data structures and
  //    does not route URT2 to baseband AT command interpreter.
  volatile uint8_t *p_uart_mode = (volatile uint8_t *)NWY_CORE_UART_MODE_ADDR;
  *p_uart_mode = NWY_UART_MODE_DATA;

  // 2. Disable internal Crane OS ROM debug logging at address 0x7e212578 (log_config).
  //    In ROM, nwy_uart_read() and nwy_uart_write() check log_config. If non-zero,
  //    ROM calls strlen() on user buffers and invokes UARTLogPrintf_Extend() with
  //    a fixed 200-byte stack buffer and character-by-character blocking loop.
  //    When bursts exceed ~190 bytes (such as 320 bytes), the 200-byte stack buffer
  //    overflows and crashes FreeRTOS! Setting log_config = 0 bypasses this completely.
  volatile uint8_t *p_log_config = (volatile uint8_t *)NWY_CORE_LOG_CONFIG_ADDR;
  *p_log_config = 0;

  g_rs485_fd = nwy_uart_open(NWY_RS485_UART_PORT, g_rs485_baudrate, FC_NONE);
  if (g_rs485_fd < 0) {
    LOGE("Failed to open RS-485 UART port '%s' (Error: %d)", NWY_RS485_UART_PORT, g_rs485_fd);
    return false;
  }

  LOGI("RS-485 UART '%s' opened successfully (FD: %d)", NWY_RS485_UART_PORT, g_rs485_fd);

  // 4. Configure 8-N-1 DCB parameters
  nwy_uartdcb_t dcb = {
      .baudrate = g_rs485_baudrate,
      .databits = 8,
      .stopbits = 1,
      .parity = PB_NONE,
      .flowctrl = FC_NONE
  };
  nwy_uart_dcb_set(g_rs485_fd, &dcb);

  // 5. Explicitly configure pure polling mode (cb = NULL)
  // This completely disables kernel UART interrupts, async work items,
  // and heap allocations, preventing RTOS watchdog panics on large packet bursts.
  nwy_uart_rx_cb_register(g_rs485_fd, NULL);

  // Flush any startup garbage/noise before operational use
  nwy_hal_rs485_flush_rx();

  return true;
}

int nwy_hal_rs485_send(const uint8_t *data, size_t len) {
  if (g_rs485_fd < 0 || !data || len == 0) {
    LOGE("RS-485 send rejected: invalid parameters");
    return -1;
  }

  // 1. Switch hardware transceiver to Transmit mode (GPIO87 = HIGH)
  nwy_hal_rs485_tx_mode();
  nwy_hal_os_thread_sleep(5); // Driver enable stabilization delay

  // 2. Transmit bytes over UART
  int written = nwy_uart_write(g_rs485_fd, (const unsigned char *)data, len);

  // 3. Wait for UART TX FIFO and shift register to fully clear differential bus
  // At 9600 baud: ~1.04 ms per character (10 bits) + 15ms margin
  uint32_t tx_delay_ms = ((len * 10 * 1000) / g_rs485_baudrate) + 15;
  nwy_hal_os_thread_sleep(tx_delay_ms);

  return written;
}

int nwy_hal_rs485_poll_read(uint8_t *buf, size_t max_len) {
  if (g_rs485_fd < 0 || !buf || max_len == 0) {
    return -1;
  }
  uint8_t chunk[64];
  memset(chunk, 0, sizeof(chunk));
  int n = nwy_uart_read(g_rs485_fd, chunk, sizeof(chunk) - 1);
  if (n > 0) {
    size_t to_copy = (size_t)n;
    if (to_copy > max_len - 1) {
      to_copy = max_len - 1;
    }
    memcpy(buf, chunk, to_copy);
    buf[to_copy] = '\0';
    return (int)to_copy;
  }
  buf[0] = '\0';
  return 0;
}

int nwy_hal_rs485_listen(uint8_t *buf, size_t max_len, uint32_t timeout_ms) {
  if (g_rs485_fd < 0 || !buf || max_len == 0) {
    return -1;
  }

  // 1. Switch to Receive mode (GPIO87 = LOW)
  nwy_hal_rs485_rx_mode();
  nwy_hal_os_thread_sleep(15); // Transceiver direction switch stabilization

  // 2. Clear UART internal RX buffer
  nwy_hal_rs485_flush_rx();
  memset(buf, 0, max_len);

  LOGI("Listening on RS-485 (URT2, DE/RE=LOW, pure polling) for %lu ms...", (unsigned long)timeout_ms);

  size_t total_received = 0;
  uint32_t wait_start_elapsed_ms = 0;
  uint32_t frame_duration_ms = 0;
  const uint32_t POLL_SLICE_MS = 10;             // 10ms poll slice (~10 bytes at 9600 baud)
  const uint32_t FRAME_QUIET_THRESHOLD_MS = 150; // 150ms without new bytes = frame complete
  const uint32_t MAX_FRAME_DURATION_MS = 5000;    // 5-second ceiling once reception begins
  uint32_t quiet_elapsed_ms = 0;

  // Dedicated hardware-FIFO bounded chunk buffer (ASR1605 FIFO depth is 64 bytes)
  // Isolates nwy_uart_read from the application accumulation buffer, ensuring
  // 100% fail-safe bounded memory operations under all FreeRTOS conditions.
  uint8_t chunk[64];

  while (1) {
    if (total_received < max_len - 1) {
      memset(chunk, 0, sizeof(chunk));
      int n = nwy_uart_read(g_rs485_fd, chunk, sizeof(chunk) - 1);
      if (n > 0) {
        size_t to_copy = (size_t)n;
        if (total_received + to_copy > max_len - 1) {
          to_copy = (max_len - 1) - total_received;
        }
        memcpy(&buf[total_received], chunk, to_copy);
        total_received += to_copy;
        buf[total_received] = '\0';
        quiet_elapsed_ms = 0; // Reset quiet timer on each chunk received
      } else {
        if (total_received > 0) {
          quiet_elapsed_ms += POLL_SLICE_MS;
          if (quiet_elapsed_ms >= FRAME_QUIET_THRESHOLD_MS) {
            // Inter-frame silence detected, entire packet received
            break;
          }
        }
      }
    } else {
      // Buffer full, cannot accept more bytes
      break;
    }

    if (total_received == 0) {
      wait_start_elapsed_ms += POLL_SLICE_MS;
      if (wait_start_elapsed_ms >= timeout_ms) {
        // Timed out waiting for initial response byte
        break;
      }
    } else {
      frame_duration_ms += POLL_SLICE_MS;
      if (frame_duration_ms >= MAX_FRAME_DURATION_MS) {
        // Safety ceiling reached for incoming frame
        break;
      }
    }

    nwy_hal_os_thread_sleep(POLL_SLICE_MS);
  }

  buf[total_received] = '\0';

  if (total_received > 0) {
    LOGI("RS-485 RX complete! Total received: %u bytes", (unsigned int)total_received);
  } else {
    LOGW("RS-485 RX: No data received within %lu ms timeout", (unsigned long)timeout_ms);
  }

  return (int)total_received;
}

void nwy_hal_rs485_deinit(void) {
  LOGI("De-initializing RS-485 interface...");

  // Close UART port descriptor cleanly
  if (g_rs485_fd >= 0) {
    nwy_uart_close(g_rs485_fd);
    g_rs485_fd = -1;
  }

  // Drive direction to Receive mode and cut transceiver power
  nwy_hal_rs485_rx_mode();
  nwy_hal_gpio_set_value(PIN_RS485_POWER_GPIO0, false);
  LOGI("Transceiver VCC Rail (GPIO00): CUT (OFF)");
}

/* =========================================================================
 * MODBUS RTU & HYDROSTATIC SENSOR IMPLEMENTATION
 * ========================================================================= */

uint16_t nwy_hal_modbus_calc_crc(const uint8_t *data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)data[pos];
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

ModbusStatusCode nwy_hal_modbus_send_receive(
    uint8_t address,
    const uint8_t *command,
    uint8_t command_length,
    uint8_t *response,
    uint8_t expected_response_length,
    uint32_t timeout_ms)
{
  if (g_rs485_fd < 0 || !command || command_length == 0 || !response || expected_response_length < 5) {
    return MODBUS_STATUS_FRAME_ERROR;
  }

  uint8_t tx_frame[32] = {0};
  tx_frame[0] = address;
  memcpy(&tx_frame[1], command, command_length);

  uint16_t tx_crc = nwy_hal_modbus_calc_crc(tx_frame, (uint16_t)(command_length + 1));
  tx_frame[command_length + 1] = (uint8_t)(tx_crc & 0x00FF);
  tx_frame[command_length + 2] = (uint8_t)((tx_crc >> 8) & 0x00FF);
  uint16_t tx_length = (uint16_t)(command_length + 3);

  // Hex dump of transmitted frame
  char tx_hex[64] = {0};
  for (uint16_t i = 0; i < tx_length && i < 16; i++) {
    char byte_str[4];
    snprintf(byte_str, sizeof(byte_str), "%02X ", tx_frame[i]);
    strncat(tx_hex, byte_str, sizeof(tx_hex) - strlen(tx_hex) - 1);
  }
  LOGI("Modbus TX Frame (%d bytes): %s", tx_length, tx_hex);

  // 1. Drain RX FIFO before transmit to eliminate line noise
  nwy_hal_rs485_flush_rx();

  // 2. Transmit mode (GPIO87 = HIGH, DE=1, /RE=1)
  nwy_hal_rs485_tx_mode();
  nwy_hal_os_thread_sleep(1); // Driver enable stabilization delay

  // 3. Write frame to UART
  int written = nwy_uart_write(g_rs485_fd, (const unsigned char *)tx_frame, tx_length);
  if (written != (int)tx_length) {
    nwy_hal_rs485_rx_mode();
    LOGE("Modbus TX write failed: %d/%d bytes", written, tx_length);
    return MODBUS_STATUS_NO_RESPONSE;
  }

  // 4. Wait for transmission to clear UART shift register
  // At 4800 baud: 8 bytes = ~16.7 ms. Add 6ms margin -> ~23 ms.
  uint32_t tx_delay_ms = ((tx_length * 10 * 1000) / g_rs485_baudrate) + 6;
  if (tx_delay_ms < 3) tx_delay_ms = 3;
  nwy_hal_os_thread_sleep(tx_delay_ms);

  // 5. Receive mode (GPIO87 = LOW, DE=0, /RE=0) immediately
  nwy_hal_rs485_rx_mode();

  // 6. Receive response bytes via polling
  memset(response, 0, expected_response_length);
  size_t total_received = 0;
  uint32_t elapsed_ms = 0;
  const uint32_t POLL_SLICE_MS = 5;

  while (elapsed_ms < timeout_ms && total_received < expected_response_length) {
    uint8_t chunk[32];
    memset(chunk, 0, sizeof(chunk));
    int n = nwy_uart_read(g_rs485_fd, chunk, sizeof(chunk) - 1);
    if (n > 0) {
      size_t to_copy = (size_t)n;
      if (total_received + to_copy > expected_response_length) {
        to_copy = expected_response_length - total_received;
      }
      memcpy(&response[total_received], chunk, to_copy);
      total_received += to_copy;
    }
    if (total_received >= expected_response_length) {
      break;
    }
    nwy_hal_os_thread_sleep(POLL_SLICE_MS);
    elapsed_ms += POLL_SLICE_MS;
  }

  if (total_received > 0) {
    char rx_hex[64] = {0};
    for (size_t i = 0; i < total_received && i < 16; i++) {
      char byte_str[4];
      snprintf(byte_str, sizeof(byte_str), "%02X ", response[i]);
      strncat(rx_hex, byte_str, sizeof(rx_hex) - strlen(rx_hex) - 1);
    }
    LOGI("Modbus RX Data (%u bytes): %s", (unsigned int)total_received, rx_hex);
  }

  if (total_received < expected_response_length) {
    LOGW("Modbus RX timeout: received %u/%u bytes in %lu ms",
         (unsigned int)total_received, (unsigned int)expected_response_length, (unsigned long)timeout_ms);
    return MODBUS_STATUS_NO_RESPONSE;
  }

  // 7. Verify Modbus CRC
  uint16_t calc_crc = nwy_hal_modbus_calc_crc(response, (uint16_t)(expected_response_length - 2));
  uint16_t rx_crc = (uint16_t)response[expected_response_length - 2] |
                    ((uint16_t)response[expected_response_length - 1] << 8);

  if (calc_crc != rx_crc) {
    LOGE("Modbus CRC mismatch: calculated 0x%04X, received 0x%04X", calc_crc, rx_crc);
    return MODBUS_STATUS_CRC_ERROR;
  }

  // 8. Verify slave address
  if (response[0] != address) {
    LOGE("Modbus address mismatch: expected 0x%02X, received 0x%02X", address, response[0]);
    return MODBUS_STATUS_FRAME_ERROR;
  }

  return MODBUS_STATUS_OK;
}

bool nwy_hal_rs485_read_hydro_sensor(nwy_hydro_sensor_data_t *sensor_out) {
  if (!sensor_out) return false;
  memset(sensor_out, 0, sizeof(*sensor_out));
  sensor_out->status = MODBUS_STATUS_NOT_READ;

  LOGI("==============================================================");
  LOGI(">>> RS-485 HYDROSTATIC LEVEL SENSOR INTERFACE (MODBUS RTU) <<<");
  LOGI("==============================================================");
  LOGI("1. Powering ON Sensor & RS-485 Transceiver at 4800 baud (GPIO00=1)...");
  if (!nwy_hal_rs485_init(4800)) {
    LOGE("Failed to initialize RS-485 interface at 4800 baud!");
    sensor_out->status = MODBUS_STATUS_FRAME_ERROR;
    return false;
  }

  // Industrial sensor requirement: allow internal transducer & boost power stabilization
  LOGI("2. Waiting 3000 ms for sensor transducer & boost power stabilization...");
  nwy_hal_os_thread_sleep(3000);

  // Command to read Register 0x0004 (Water Level): Function 0x03, Reg 0x0004, Count 0x0001
  const uint8_t modbus_cmd[5] = {0x03, 0x00, 0x04, 0x00, 0x01};
  uint8_t rx_buf[16] = {0};

  ModbusStatusCode status = MODBUS_STATUS_NO_RESPONSE;
  const int MAX_RETRIES = 5;

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    memset(rx_buf, 0, sizeof(rx_buf));
    LOGI("3. Querying Hydrostatic Sensor [Attempt %d/%d]: Slave=0x01, Reg=0x0004 (4800 baud)...",
         attempt, MAX_RETRIES);

    status = nwy_hal_modbus_send_receive(
        0x01, modbus_cmd, sizeof(modbus_cmd), rx_buf, 7, 1500);

    if (status == MODBUS_STATUS_OK) {
      if (rx_buf[1] == 0x03 && rx_buf[2] == 0x02) {
        uint16_t raw_mm = ((uint16_t)rx_buf[3] << 8) | (uint16_t)rx_buf[4];
        sensor_out->raw_register = raw_mm;
        sensor_out->level_mm = (float)raw_mm;
        sensor_out->level_m  = sensor_out->level_mm / 1000.0f;
        sensor_out->is_valid = true;
        sensor_out->status   = MODBUS_STATUS_OK;
        LOGI(">>> HYDROSTATIC LEVEL SENSOR ACQUISITION SUCCESS [Attempt %d/%d]! <<<",
             attempt, MAX_RETRIES);
        LOGI("    Raw Register (0x0004) : 0x%04X (%u)", raw_mm, raw_mm);
        LOGI("    Water Level (mm)      : %.1f mm", sensor_out->level_mm);
        LOGI("    Water Level (meters)  : %.3f m", sensor_out->level_m);
        break;
      } else {
        LOGW("Invalid response format [Attempt %d]: Func=0x%02X, Bytes=%u",
             attempt, rx_buf[1], rx_buf[2]);
        status = MODBUS_STATUS_FRAME_ERROR;
      }
    } else {
      LOGW("Modbus query attempt %d/%d failed (Status: %d)", attempt, MAX_RETRIES, (int)status);
    }

    if (attempt < MAX_RETRIES) {
      nwy_hal_os_thread_sleep(500); // 500 ms inter-retry recovery delay
    }
  }

  // Power off sensor and transceiver immediately to conserve battery
  LOGI("4. Powering OFF Sensor & Transceiver (GPIO00=0) to preserve battery...");
  nwy_hal_rs485_deinit();

  sensor_out->status = status;
  return sensor_out->is_valid;
}
