#include "hal_uart.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include "nwy_usb_serial.h"
#include "nwy_dev_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UART_BAUD 115200

// Handles for the physical UART ports supported by the Neoway N706B SDK
static int g_fd_urt1 = -1; // "URT1": Pins 38 & 39 (EVB Header 13, DBG)
static int g_fd_urt2 = -1; // "URT2": Pins 17 & 18 (EVB Header 11, MAIN)

static volatile uint32_t g_ping_counter = 0;
static volatile uint32_t g_rx_count_urt1 = 0;
static volatile uint32_t g_rx_count_urt2 = 0;

// Helper: send formatted string to USB virtual serial CDC
static void usb_log(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len > 0) {
    nwy_usb_serial_send((void *)buf, len);
    nwy_usb_serial_send((void *)"\r\n", 2);
  }
}

// Broadcast message across all currently open ports and USB
static void broadcast_all(const char *str) {
  if (!str) return;
  size_t len = strlen(str);
  if (g_fd_urt1 >= 0) hal_uart_write(g_fd_urt1, (const uint8_t *)str, len);
  if (g_fd_urt2 >= 0) hal_uart_write(g_fd_urt2, (const uint8_t *)str, len);
  usb_log("%s", str);
}

// Common command interpreter for any receiving port
static void handle_command(int fd, const char *port_label, char cmd) {
  switch (cmd) {
  case 'v':
  case 'V': {
    int vbat_mv = 0;
    nwy_pm_vbat_voltage_get(&vbat_mv);
    hal_uart_printf(fd, ">>> [%s CMD] Battery Rail VBAT = %d mV\r\n", port_label, vbat_mv);
    break;
  }
  case 't':
  case 'T': {
    nwy_time_t curr_time;
    int tz = 0;
    memset(&curr_time, 0, sizeof(curr_time));
    if (nwy_date_get(&curr_time, &tz) == 0) {
      hal_uart_printf(fd, ">>> [%s CMD] RTC Time: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                      port_label, curr_time.year, curr_time.mon, curr_time.day,
                      curr_time.hour, curr_time.min, curr_time.sec);
    }
    break;
  }
  case 'r':
  case 'R': {
    broadcast_all("\r\n>>> [SYSTEM] Rebooting module in 1 second...\r\n");
    nwy_thread_sleep(1000);
    nwy_pm_ctrl(3); // Soft reboot
    break;
  }
  case 'h':
  case 'H':
  default: {
    hal_uart_printf(fd, "\r\n--- [%s MENU] ---\r\n"
                        "  v - Query VBAT voltage\r\n"
                        "  t - Query RTC time\r\n"
                        "  r - Reboot module\r\n"
                        "  h - Show this menu\r\n\r\n", port_label);
    break;
  }
  }
}

// RX Callbacks for each individual port
static void on_urt1_rx(const char *str, uint32_t length) {
  if (length == 0 || !str) return;
  g_rx_count_urt1 += length;
  hal_uart_printf(g_fd_urt1, "\r\n[URT1 ECHO] Received: '%.*s' (%u bytes)\r\n",
                  (int)length, str, (unsigned int)length);
  handle_command(g_fd_urt1, "URT1", str[0]);
}

static void on_urt2_rx(const char *str, uint32_t length) {
  if (length == 0 || !str) return;
  g_rx_count_urt2 += length;
  hal_uart_printf(g_fd_urt2, "\r\n[URT2 ECHO] Received: '%.*s' (%u bytes)\r\n",
                  (int)length, str, (unsigned int)length);
  handle_command(g_fd_urt2, "URT2", str[0]);
}

// Main monitor thread
static void main_monitor_task(void *param) {
  // Allow system power rails and USB to fully stabilize
  nwy_thread_sleep(2000);

  usb_log("Uart_Test: Initializing multi-port UART test harness...");

  // 1. Open URT1 (Pins 38/39, Header 13 on EVB)
  g_fd_urt1 = hal_uart_open("URT1", TEST_UART_BAUD);
  if (g_fd_urt1 >= 0) {
    hal_uart_register_rx_cb(g_fd_urt1, on_urt1_rx);
    usb_log("URT1 opened successfully (fd=%d)", g_fd_urt1);
  } else {
    usb_log("URT1 open failed (fd=%d)", g_fd_urt1);
  }

  // 2. Open URT2 (Pins 17/18, Header 11 on EVB)
  g_fd_urt2 = hal_uart_open("URT2", TEST_UART_BAUD);
  if (g_fd_urt2 >= 0) {
    hal_uart_register_rx_cb(g_fd_urt2, on_urt2_rx);
    usb_log("URT2 opened successfully (fd=%d)", g_fd_urt2);
  } else {
    usb_log("URT2 open failed (fd=%d)", g_fd_urt2);
  }

  // Initial Startup Banners
  broadcast_all("\r\n=======================================================\r\n");
  broadcast_all("   NEOWAY N706B MULTI-PORT UART TEST HARNESS          \r\n");
  broadcast_all("=======================================================\r\n");

  if (g_fd_urt1 >= 0) {
    hal_uart_printf(g_fd_urt1, "[URT1 ACTIVE] Port URT1 (Pins 38/39, EVB Header 13) is ONLINE!\r\n");
  }
  if (g_fd_urt2 >= 0) {
    hal_uart_printf(g_fd_urt2, "[URT2 ACTIVE] Port URT2 (Pins 17/18, EVB Header 11) is ONLINE!\r\n");
  }

  int vbat_mv = 0;
  nwy_pm_vbat_voltage_get(&vbat_mv);
  char info_msg[128];
  snprintf(info_msg, sizeof(info_msg), "Status: VBAT=%d mV | URT1_fd=%d | URT2_fd=%d\r\n\r\n",
           vbat_mv, g_fd_urt1, g_fd_urt2);
  broadcast_all(info_msg);

  // Periodic ping loop: transmits every 1 second across active ports
  while (1) {
    g_ping_counter++;
    nwy_pm_vbat_voltage_get(&vbat_mv);

    // Ping on URT1 (DBG)
    if (g_fd_urt1 >= 0) {
      hal_uart_printf(g_fd_urt1, "[URT1_DBG #%04lu] Ping! VBAT=%d mV (RX=%lu)\r\n",
                      (unsigned long)g_ping_counter, vbat_mv, (unsigned long)g_rx_count_urt1);
    }

    // Ping on URT2 (MAIN)
    if (g_fd_urt2 >= 0) {
      hal_uart_printf(g_fd_urt2, "[URT2_MAIN #%04lu] Ping! VBAT=%d mV (RX=%lu)\r\n",
                      (unsigned long)g_ping_counter, vbat_mv, (unsigned long)g_rx_count_urt2);
    }

    usb_log("[PING #%lu] Sent on active ports", (unsigned long)g_ping_counter);
    nwy_thread_sleep(1000);
  }
}

// Entry point called by Crane RTOS
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_thread_sleep(1000);

  char version[] = "\"Uart_Test_V1.0\"";
  nwy_dm_app_version_set(version, strlen(version));

  nwy_osi_thread_t th = NULL;
  // NOTE: event_count MUST be non-zero (50) for nwy_thread_create to succeed!
  nwy_error_e ret = nwy_thread_create(&th, "uart_test_task", NWY_OSI_PRIORITY_NORMAL,
                                      main_monitor_task, NULL, 50, 1024 * 4, NULL);
  if (ret != NWY_SUCCESS) {
    usb_log("Failed to create uart_test_task: ret=%d", ret);
  }
  return 0;
}

void appimg_exit(void) {
  if (g_fd_urt1 >= 0) hal_uart_close(g_fd_urt1);
  if (g_fd_urt2 >= 0) hal_uart_close(g_fd_urt2);
}
