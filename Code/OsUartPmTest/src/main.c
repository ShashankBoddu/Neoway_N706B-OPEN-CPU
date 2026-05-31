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

#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

static int g_uart_fd = -1;
static nwy_osi_timer_t g_heartbeat_timer = NULL;
static nwy_osi_semaphore_t g_test_sem = NULL;
static nwy_osi_mutex_t g_test_mutex = NULL;
static int g_heartbeat_count = 0;

static void uart_send_string(const char *str) {
  if (g_uart_fd >= 0 && str) {
    nwy_hal_uart_write(g_uart_fd, (const uint8_t *)str, strlen(str));
  }
}

// Timer Callback
static void heartbeat_timer_callback(void *ctx) {
  g_heartbeat_count++;
  char log_buf[64];
  snprintf(log_buf, sizeof(log_buf), "\r\n[TIMER] Heartbeat count: %d\r\n",
           g_heartbeat_count);
  uart_send_string(log_buf);
}

// UART RX Callback
static void test_uart_rx_cb(const char *str, uint32_t length) {
  if (length == 0 || !str)
    return;

  char cmd = str[0];
  char out_buf[128];

  switch (cmd) {
  case 'r': // Reboot
    uart_send_string("\r\n[CMD] Rebooting system...\r\n");
    nwy_hal_os_thread_sleep(1000);
    nwy_hal_pm_reboot();
    break;

  case 'p': // Power Off
    uart_send_string("\r\n[CMD] Powering off system...\r\n");
    nwy_hal_os_thread_sleep(1000);
    nwy_hal_pm_power_off();
    break;

  case 'v': // Get VBAT
  {
    int vbat = 0;
    if (nwy_hal_pm_get_vbat(&vbat)) {
      snprintf(out_buf, sizeof(out_buf), "\r\n[CMD] Battery Voltage: %d mV\r\n",
               vbat);
    } else {
      snprintf(out_buf, sizeof(out_buf),
               "\r\n[CMD] Failed to get battery voltage\r\n");
    }
    uart_send_string(out_buf);
  } break;

  case 't': // Get Time
  {
    nwy_time_t curr_time;
    int timezone = 0;
    memset(&curr_time, 0, sizeof(curr_time));
    if (nwy_hal_pm_get_time(&curr_time, &timezone)) {
      snprintf(out_buf, sizeof(out_buf),
               "\r\n[CMD] Date: %04d-%02d-%02d %02d:%02d:%02d GMT%+d\r\n",
               curr_time.year, curr_time.mon, curr_time.day, curr_time.hour,
               curr_time.min, curr_time.sec, timezone);
    } else {
      snprintf(out_buf, sizeof(out_buf),
               "\r\n[CMD] Failed to get RTC time\r\n");
    }
    uart_send_string(out_buf);
  } break;

  case 's': // Semaphore Test
    uart_send_string("\r\n[CMD] Testing Semaphore...\r\n");
    if (g_test_sem) {
      // Try to acquire non-blocking
      if (nwy_hal_os_semaphore_acquire(g_test_sem, 0)) {
        uart_send_string("[SEM] Semaphore acquired successfully!\r\n");
      } else {
        uart_send_string(
            "[SEM] Semaphore unavailable (count is 0). Releasing...\r\n");
        nwy_hal_os_semaphore_release(g_test_sem);
        uart_send_string("[SEM] Semaphore released.\r\n");
      }
    }
    break;

  case 'm': // Mutex Test
    uart_send_string("\r\n[CMD] Testing Mutex...\r\n");
    if (g_test_mutex) {
      if (nwy_hal_os_mutex_lock(g_test_mutex, 1000)) {
        uart_send_string(
            "[MUTEX] Locked. Performing critical operations...\r\n");
        nwy_hal_os_thread_sleep(500);
        nwy_hal_os_mutex_unlock(g_test_mutex);
        uart_send_string("[MUTEX] Unlocked.\r\n");
      } else {
        uart_send_string("[MUTEX] Lock timeout!\r\n");
      }
    }
    break;

  case 'h': // Help Menu
  default:
    uart_send_string("\r\n--- Supported Commands ---\r\n"
                     " r - Reboot Module\r\n"
                     " p - Power Off Module\r\n"
                     " v - Get Battery Voltage\r\n"
                     " t - Get System Time\r\n"
                     " s - Test OS Semaphores\r\n"
                     " m - Test OS Mutexes\r\n"
                     " h - Show this menu\r\n"
                     "--------------------------\r\n");
    break;
  }
}

// Background monitoring task
static void main_monitor_task(void *param) {
  nwy_hal_os_thread_sleep(3000); // Allow UART and system stability

  usb_printf("OsUartPmTest: Spawning test UART interface...");
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd < 0) {
    usb_printf("OsUartPmTest: Failed to open UART %s, error %d", TEST_UART_PORT,
               g_uart_fd);
    return;
  }

  usb_printf("OsUartPmTest: UART %s opened successfully (fd=%d)",
             TEST_UART_PORT, g_uart_fd);
  nwy_hal_uart_register_rx_cb(g_uart_fd, test_uart_rx_cb);

  // Print Initial Greeting
  uart_send_string("\r\n==============================================\r\n");
  uart_send_string("  NEOWAY N706B OPENCPU - OS / UART / PM TEST  \r\n");
  uart_send_string("==============================================\r\n");

  char reason_buf[64];
  snprintf(reason_buf, sizeof(reason_buf), "Boot Reason: %s\r\n",
           nwy_hal_pm_get_boot_reason_str());
  uart_send_string(reason_buf);

  // Create synchronization resources
  nwy_hal_os_semaphore_create(&g_test_sem, 1, 1);
  nwy_hal_os_mutex_create(&g_test_mutex);

  // Create and start periodic heartbeat timer
  if (nwy_hal_os_timer_create(&g_heartbeat_timer, heartbeat_timer_callback,
                              NULL)) {
    nwy_hal_os_timer_start(g_heartbeat_timer, 2000, NWY_HAL_OS_TIMER_PERIODIC);
    uart_send_string(
        "[INIT] Heartbeat timer created and started (2s interval).\r\n");
  } else {
    uart_send_string("[INIT] Failed to create heartbeat timer.\r\n");
  }

  uart_send_string("Press 'h' for command options.\r\n");

  while (1) {
    nwy_hal_os_thread_sleep(10000);
    usb_printf("OsUartPmTest: main thread looping...");
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
  if (g_uart_fd >= 0) {
    nwy_hal_uart_close(g_uart_fd);
  }
}
