#include "hal/os/nwy_hal_os.h"
#include "hal/pm/nwy_hal_pm.h"
#include "hal/uart/nwy_hal_uart.h"
#include "nwy_log_api.h"
#include "nwy_osi_api.h"
#include "nwy_usb_serial.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usb_printf(const char *fmt, ...) {
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

#define DBG_UART_PORT "URT1"
#define MAIN_UART_PORT "URT2"
#define TEST_UART_BAUD 115200

static int g_dbg_uart_fd = -1;
static int g_main_uart_fd = -1;

static nwy_osi_timer_t g_heartbeat_timer = NULL;
static nwy_osi_semaphore_t g_test_sem = NULL;
static nwy_osi_mutex_t g_test_mutex = NULL;
static int g_heartbeat_count = 0;

static void broadcast_string(const char *str) {
  if (!str) return;
  size_t len = strlen(str);
  if (g_main_uart_fd >= 0) {
    nwy_hal_uart_write(g_main_uart_fd, (const uint8_t *)str, len);
  }
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_write(g_dbg_uart_fd, (const uint8_t *)str, len);
  }
  usb_printf("%s", str);
}

// Timer Callback: Broadcasts heartbeat every 1 second
static void heartbeat_timer_callback(void *ctx) {
  g_heartbeat_count++;
  int vbat_mv = 0;
  nwy_hal_pm_get_vbat(&vbat_mv);

  char ping_buf[160];
  snprintf(ping_buf, sizeof(ping_buf),
           "[MAIN_URT2 #%04d] HELLO FROM PINS 17/18! VBAT=%d mV\r\n",
           g_heartbeat_count, vbat_mv);

  // Send packet out of MAIN UART (Pins 17/18, EVB Header J606)
  if (g_main_uart_fd >= 0) {
    nwy_hal_uart_write(g_main_uart_fd, (const uint8_t *)ping_buf, strlen(ping_buf));
  }

  // Also log status to DBG UART (Pins 38/39, EVB Header J607) and USB
  char mon_buf[160];
  snprintf(mon_buf, sizeof(mon_buf),
           "[DBG_URT1] Ping #%04d sent out on MAIN_URT2 (Pins 17/18).\r\n",
           g_heartbeat_count);
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_write(g_dbg_uart_fd, (const uint8_t *)mon_buf, strlen(mon_buf));
  }
  usb_printf("[PING #%d] Sent to MAIN_URT2", g_heartbeat_count);
}

// MAIN UART (URT2, Pins 17/18) RX Callback
static void main_uart_rx_cb(const char *str, uint32_t length) {
  if (length == 0 || !str) return;

  char cmd = str[0];
  char out_buf[160];

  snprintf(out_buf, sizeof(out_buf),
           "\r\n[MAIN_URT2 ECHO] Received character: '%c' (0x%02X) on Module Pin 18 (MAIN_RXD)!\r\n",
           (cmd >= 32 && cmd <= 126) ? cmd : '.', (unsigned char)cmd);
  broadcast_string(out_buf);

  switch (cmd) {
  case 'r': // Reboot
    broadcast_string("\r\n[CMD] Rebooting system...\r\n");
    nwy_hal_os_thread_sleep(1000);
    nwy_hal_pm_reboot();
    break;

  case 'p': // Power Off
    broadcast_string("\r\n[CMD] Powering off system...\r\n");
    nwy_hal_os_thread_sleep(1000);
    nwy_hal_pm_power_off();
    break;

  case 's': // Semaphore test
    broadcast_string("\r\n[CMD] Testing Semaphore...\r\n");
    if (g_test_sem) {
      if (nwy_hal_os_semaphore_acquire(g_test_sem, 0)) {
        broadcast_string("[SEM] Semaphore acquired successfully!\r\n");
      } else {
        broadcast_string("[SEM] Semaphore unavailable (count=0). Releasing...\r\n");
        nwy_hal_os_semaphore_release(g_test_sem);
        broadcast_string("[SEM] Semaphore released.\r\n");
      }
    }
    break;

  case 'm': // Mutex test
    broadcast_string("\r\n[CMD] Testing Mutex...\r\n");
    if (g_test_mutex) {
      if (nwy_hal_os_mutex_lock(g_test_mutex, 1000)) {
        broadcast_string("[MUTEX] Locked. Performing critical operations...\r\n");
        nwy_hal_os_thread_sleep(500);
        nwy_hal_os_mutex_unlock(g_test_mutex);
        broadcast_string("[MUTEX] Unlocked.\r\n");
      } else {
        broadcast_string("[MUTEX] Lock timeout!\r\n");
      }
    }
    break;

  case 'v': // Battery status
    {
      int vbat = 0;
      nwy_hal_pm_get_vbat(&vbat);
      char vbuf[64];
      snprintf(vbuf, sizeof(vbuf), "\r\n[CMD] Current VBAT = %d mV\r\n", vbat);
      broadcast_string(vbuf);
    }
    break;

  case 'h': // Help Menu
  default:
    broadcast_string("\r\n=========================================\r\n"
                     " NEOWAY N706B MAIN UART (URT2) COMMAND MENU \r\n"
                     "=========================================\r\n"
                     "  'r' : Soft Reboot Module\r\n"
                     "  'p' : Power Off Module\r\n"
                     "  's' : Test OS Semaphore\r\n"
                     "  'm' : Test OS Mutex\r\n"
                     "  'v' : Query Battery Voltage (VBAT)\r\n"
                     "  'h' : Show This Help Menu\r\n"
                     "=========================================\r\n\r\n");
    break;
  }
}

// DBG UART (URT1, Pins 38/39) RX Callback
static void dbg_uart_rx_cb(const char *str, uint32_t length) {
  if (length == 0 || !str) return;
  // Route to the same command interpreter
  main_uart_rx_cb(str, length);
}

// Background monitoring task
static void main_monitor_task(void *param) {
  nwy_hal_os_thread_sleep(2000); // Allow UART and system stability

  usb_printf("OsUartPmTest: Starting Multi-UART Test Harness...");

  // 1. Open DBG_UART_PORT (URT1, Pins 38/39, EVB Header J607) for diagnostics
  g_dbg_uart_fd = nwy_hal_uart_open(DBG_UART_PORT, TEST_UART_BAUD);
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_register_rx_cb(g_dbg_uart_fd, dbg_uart_rx_cb);
    usb_printf("OsUartPmTest: DBG UART %s opened (fd=%d)", DBG_UART_PORT, g_dbg_uart_fd);
  }

  // 2. Open MAIN_UART_PORT (URT2, Pins 17/18, EVB Header J606) as PRIMARY test subject
  g_main_uart_fd = nwy_hal_uart_open(MAIN_UART_PORT, TEST_UART_BAUD);
  if (g_main_uart_fd >= 0) {
    nwy_hal_uart_register_rx_cb(g_main_uart_fd, main_uart_rx_cb);
    usb_printf("OsUartPmTest: MAIN UART %s opened (fd=%d)", MAIN_UART_PORT, g_main_uart_fd);
  } else {
    usb_printf("OsUartPmTest: FAILED to open MAIN UART %s (err=%d)", MAIN_UART_PORT, g_main_uart_fd);
  }

  // 3. Print Initial Diagnostic Banner on all active channels
  broadcast_string("\r\n");
  broadcast_string("====================================================================\r\n");
  broadcast_string("  NEOWAY N706B OPENCPU - MAIN UART (PINS 17/18) TEST HARNESS        \r\n");
  broadcast_string("====================================================================\r\n");
  broadcast_string("  Primary Port : URT2 (Module Pin 17 MAIN_TXD / Pin 18 MAIN_RXD)\r\n");
  broadcast_string("  Debug Mirror : URT1 (Module Pin 38 DBG_RXD / Pin 39 DBG_TXD)\r\n");
  broadcast_string("  Baud Rate    : 115200 baud, 8-N-1 (Flow Control: NONE)\r\n");
  broadcast_string("--------------------------------------------------------------------\r\n");
  broadcast_string("  >> HARDWARE WIRING ON MINI EVB CONNECTOR J606 (UART1) <<\r\n");
  broadcast_string("     Pin 2: MAIN_RXD -> MODULE PIN 18 (Input)  *** CONNECT PC TX HERE ***\r\n");
  broadcast_string("     Pin 3: MAIN_TXD -> MODULE PIN 17 (Output) *** CONNECT PC RX HERE ***\r\n");
  broadcast_string("     Pin 4: GND      -> CONNECT PC GND HERE\r\n");
  broadcast_string("====================================================================\r\n\r\n");

  char boot_buf[64];
  snprintf(boot_buf, sizeof(boot_buf), "Boot Reason: %s\r\n", nwy_hal_pm_get_boot_reason_str());
  broadcast_string(boot_buf);

  // Create synchronization resources
  nwy_hal_os_semaphore_create(&g_test_sem, 1, 1);
  nwy_hal_os_mutex_create(&g_test_mutex);

  // Create and start periodic heartbeat timer (1-second interval)
  if (nwy_hal_os_timer_create(&g_heartbeat_timer, heartbeat_timer_callback, NULL)) {
    nwy_hal_os_timer_start(g_heartbeat_timer, 1000, NWY_HAL_OS_TIMER_PERIODIC);
    broadcast_string("[INIT] Heartbeat timer active (1-second interval broadcast).\r\n");
  } else {
    broadcast_string("[WARN] Failed to create heartbeat timer!\r\n");
  }

  // Main monitor loop: logs runtime telemetry
  uint32_t seconds = 0;
  while (1) {
    nwy_hal_os_thread_sleep(5000);
    seconds += 5;

    int vbat = 0;
    nwy_hal_pm_get_vbat(&vbat);

    char stat_buf[128];
    snprintf(stat_buf, sizeof(stat_buf),
             "[MONITOR #%lu] Core running normally. Uptime: %lu s | VBAT: %d mV\r\n",
             (unsigned long)(seconds / 5), (unsigned long)seconds, vbat);
    broadcast_string(stat_buf);
  }
}

// Application Entry
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_thread_sleep(1000);
  usb_printf("OsUartPmTest: Application entered...");

  nwy_osi_thread_t monitor_thread = NULL;
  bool ok = nwy_hal_os_thread_create(&monitor_thread, "main_monitor",
                                     main_monitor_task, NULL,
                                     NWY_OSI_PRIORITY_NORMAL, 1024 * 4);
  usb_printf("OsUartPmTest: Thread create status = %d", ok);

  return 0;
}

// Application Exit
void appimg_exit(void) {
  usb_printf("OsUartPmTest: Application exited");

  if (g_heartbeat_timer) {
    nwy_hal_os_timer_stop(g_heartbeat_timer);
    nwy_hal_os_timer_delete(g_heartbeat_timer);
  }
  if (g_test_sem) {
    nwy_hal_os_semaphore_delete(g_test_sem);
  }
  if (g_test_mutex) {
    nwy_hal_os_mutex_delete(g_test_mutex);
  }
  if (g_main_uart_fd >= 0) {
    nwy_hal_uart_close(g_main_uart_fd);
  }
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_close(g_dbg_uart_fd);
  }
}
