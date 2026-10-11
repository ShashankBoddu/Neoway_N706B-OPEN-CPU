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

#define NWY_CORE_UART_MODE_ADDR 0x7e22e3c0
#define RS485_INTERNAL_BUF_SIZE 1024

static int g_rs485_fd = -1;
static uint32_t g_rs485_baudrate = 9600;

// High-capacity RX accumulation buffer filled by kernel UART ISR
static uint8_t s_rs485_rx_fifo[RS485_INTERNAL_BUF_SIZE];
static volatile size_t s_rs485_rx_count = 0;
static volatile bool s_rx_capturing = false;

static void rs485_uart_rx_isr(const char *str, uint32_t length) {
  // Discard incoming bytes if capture window is not armed or parameters are invalid
  if (!s_rx_capturing || !str || length == 0) {
    return;
  }

  // Fast, bounds-checked accumulation into static buffer with saturation safety
  if (s_rs485_rx_count < sizeof(s_rs485_rx_fifo) - 1) {
    size_t space_left = sizeof(s_rs485_rx_fifo) - 1 - s_rs485_rx_count;
    size_t to_copy = (length < space_left) ? (size_t)length : space_left;
    memcpy(&s_rs485_rx_fifo[s_rs485_rx_count], str, to_copy);
    s_rs485_rx_count += to_copy;
    s_rs485_rx_fifo[s_rs485_rx_count] = '\0';
  }
}

void nwy_hal_rs485_tx_mode(void) {
  // Disable RX capture during transmission to prevent loopback/noise accumulation
  s_rx_capturing = false;
  // GPIO87 controls Q7 level shifter to RS_De (RE#/DE):
  // HIGH (1.8V): Q7 OFF -> RS_De pulled HIGH to VBAT (DE=1, RE#=1: Transmit Mode)
  nwy_hal_gpio_set_value(PIN_RS485_DIR_GPIO87, true);
}

void nwy_hal_rs485_rx_mode(void) {
  // LOW (0.0V): Q7 ON -> RS_De pulled LOW to GND (DE=0, RE#=0: Receive Mode)
  nwy_hal_gpio_set_value(PIN_RS485_DIR_GPIO87, false);
}

void nwy_hal_rs485_flush_rx(void) {
  // 1. Disarm capture while clearing
  s_rx_capturing = false;

  // 2. Clear software buffer and count
  s_rs485_rx_count = 0;
  memset(s_rs485_rx_fifo, 0, sizeof(s_rs485_rx_fifo));

  // 3. Drain low-level driver buffer and FIFO if port is open
  if (g_rs485_fd >= 0) {
    uint8_t trash[128];
    while (nwy_uart_read(g_rs485_fd, trash, sizeof(trash)) > 0) {
      // drain residual bytes
    }
  }

  // 4. Allow any in-flight UART ISR interrupts or transients to settle
  nwy_hal_os_thread_sleep(20);

  // 5. Reset counter again after transient settling
  s_rs485_rx_count = 0;
  memset(s_rs485_rx_fifo, 0, sizeof(s_rs485_rx_fifo));
}

bool nwy_hal_rs485_init(uint32_t baudrate) {
  g_rs485_baudrate = (baudrate > 0) ? baudrate : 9600;
  LOGI("Initializing RS-485 on %s at %lu baud...",
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
    s_rx_capturing = false;
    nwy_hal_os_thread_sleep(10);
    nwy_uart_close(g_rs485_fd);
    g_rs485_fd = -1;
  }

  // Critical Crane OS Configuration:
  // Enforce global uartMode = NWY_UART_MODE_DATA (1) at address 0x7e22e3c0.
  // This ensures nwy_uart_open initializes driver data structures and
  // does not route URT2 to baseband AT command interpreter.
  volatile uint8_t *p_uart_mode = (volatile uint8_t *)NWY_CORE_UART_MODE_ADDR;
  *p_uart_mode = NWY_UART_MODE_DATA;

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

  // 5. Register native asynchronous callback to safely capture incoming frames
  // Note: nwy_uart_rx_frame_timeout_set() is intentionally NOT called.
  // The SDK implementation of that function has a firmware branch condition bug
  // that leaves the timer callback uninitialized, causing a hard panic reset.
  // Native ISR accumulation delivers chunks reliably and safely.
  s_rx_capturing = false;
  s_rs485_rx_count = 0;
  memset(s_rs485_rx_fifo, 0, sizeof(s_rs485_rx_fifo));
  nwy_error_e cb_err = nwy_uart_rx_cb_register(g_rs485_fd, rs485_uart_rx_isr);
  if (cb_err != NWY_SUCCESS) {
    LOGE("Failed to register RS-485 RX callback (Error: %d)", cb_err);
    return false;
  }

  // Flush any startup garbage/noise before operational use
  nwy_hal_rs485_flush_rx();

  return true;
}

int nwy_hal_rs485_send(const uint8_t *data, size_t len) {
  if (g_rs485_fd < 0 || !data || len == 0) {
    LOGE("RS-485 send rejected: invalid parameters");
    return -1;
  }

  // 1. Switch hardware transceiver to Transmit mode (GPIO87 = HIGH, RX capture disarmed)
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
  if (!buf || max_len == 0) {
    return -1;
  }
  size_t count = s_rs485_rx_count;
  if (count == 0) return 0;
  size_t copy_len = (count < max_len - 1) ? count : (max_len - 1);
  memcpy(buf, s_rs485_rx_fifo, copy_len);
  buf[copy_len] = '\0';
  s_rs485_rx_count = 0;
  return (int)copy_len;
}

int nwy_hal_rs485_listen(uint8_t *buf, size_t max_len, uint32_t timeout_ms) {
  if (g_rs485_fd < 0 || !buf || max_len == 0) {
    return -1;
  }

  // 1. Switch to Receive mode (GPIO87 = LOW)
  nwy_hal_rs485_rx_mode();
  nwy_hal_os_thread_sleep(20); // Driver disable and transceiver RO stabilization delay

  // 2. Clear UART internal RX buffer and reset accumulation state
  nwy_hal_rs485_flush_rx();
  memset(buf, 0, max_len);

  // 3. Arm the RX capture engine
  s_rx_capturing = true;

  LOGI("Listening on RS-485 (URT2, DE/RE=LOW) for %lu ms...", (unsigned long)timeout_ms);

  uint32_t elapsed = 0;
  const uint32_t POLL_SLICE_MS = 20;
  const uint32_t FRAME_QUIET_THRESHOLD_MS = 150; // 150 ms silence window ensures full multi-chunk frame delivery
  uint32_t quiet_elapsed_ms = 0;
  size_t last_recorded_count = 0;

  while (elapsed < timeout_ms) {
    size_t current_count = s_rs485_rx_count;

    if (current_count > 0) {
      if (current_count != last_recorded_count) {
        // New bytes arrived! Reset quiet detection timer
        last_recorded_count = current_count;
        quiet_elapsed_ms = 0;
      } else {
        // No new bytes arrived during this slice, accumulate quiet time
        quiet_elapsed_ms += POLL_SLICE_MS;
        if (quiet_elapsed_ms >= FRAME_QUIET_THRESHOLD_MS) {
          // Bus is quiet for full 150 ms after bytes arrived: frame transmission is complete
          break;
        }
      }
    }

    nwy_hal_os_thread_sleep(POLL_SLICE_MS);
    elapsed += POLL_SLICE_MS;
  }

  // 4. Disarm capture to prevent concurrent ISR modification while copying
  s_rx_capturing = false;

  size_t total_received = s_rs485_rx_count;
  size_t copy_len = (total_received < max_len - 1) ? total_received : (max_len - 1);

  if (copy_len > 0) {
    memcpy(buf, s_rs485_rx_fifo, copy_len);
    buf[copy_len] = '\0';
    LOGI("RS-485 RX complete! Total received: %u bytes: '%s'", (unsigned int)copy_len, (char *)buf);
  } else {
    buf[0] = '\0';
    LOGW("RS-485 RX: No data received within %lu ms timeout", (unsigned long)timeout_ms);
  }

  return (int)copy_len;
}

void nwy_hal_rs485_deinit(void) {
  LOGI("De-initializing RS-485 interface...");

  // 1. Disarm RX capture so no callbacks process data during teardown
  s_rx_capturing = false;
  nwy_hal_os_thread_sleep(20);

  // 2. Close UART port descriptor cleanly (masks hardware interrupts)
  if (g_rs485_fd >= 0) {
    nwy_uart_close(g_rs485_fd);
    g_rs485_fd = -1;
  }

  // 3. Drive direction to Receive mode and cut transceiver power
  nwy_hal_rs485_rx_mode();
  nwy_hal_gpio_set_value(PIN_RS485_POWER_GPIO0, false);
  LOGI("Transceiver VCC Rail (GPIO00): CUT (OFF)");
}

