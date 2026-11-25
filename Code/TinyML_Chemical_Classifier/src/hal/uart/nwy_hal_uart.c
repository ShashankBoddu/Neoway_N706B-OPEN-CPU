#include "hal/uart/nwy_hal_uart.h"
#include "nwy_uart_api.h"
#include "nwy_osi_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_log_fd = -1;
static nwy_hal_log_level_t g_current_log_level = NWY_HAL_LOG_INFO;
static bool g_log_timestamp = false;

/* Mutex for thread-safe concurrent UART output across all FreeRTOS tasks */
static nwy_osi_mutex_t  s_uart_mutex = NULL;
static nwy_osi_thread_t s_uart_owner = NULL;
static uint32_t         s_uart_recursion = 0;

static bool uart_lock(uint32_t timeout_ms)
{
    if (s_uart_mutex == NULL) {
        nwy_sdk_mutex_create(&s_uart_mutex);
        if (s_uart_mutex == NULL) return true; /* Fail-open if RTOS heap exhausted */
    }

    nwy_osi_thread_t cur = nwy_thread_get_current();
    if (s_uart_owner == cur) {
        s_uart_recursion++;
        return true;
    }

    if (nwy_sdk_mutex_lock(s_uart_mutex, (int)timeout_ms) == NWY_SUCCESS) {
        s_uart_owner = cur;
        s_uart_recursion = 1;
        return true;
    }
    return false;
}

static void uart_unlock(void)
{
    if (s_uart_mutex == NULL) return;

    nwy_osi_thread_t cur = nwy_thread_get_current();
    if (s_uart_owner == cur) {
        if (s_uart_recursion > 1) {
            s_uart_recursion--;
        } else {
            s_uart_recursion = 0;
            s_uart_owner = NULL;
            nwy_sdk_mutex_unlock(s_uart_mutex);
        }
    }
}

static int resolve_target_fd(int fd)
{
    /* If caller passes <= 0 or 1, route to active default log descriptor */
    if (fd <= 0 || (fd == 1 && g_log_fd >= 0)) {
        if (g_log_fd >= 0) {
            return g_log_fd;
        }
        /* Auto-open fallback to default debug UART "URT1" */
        g_log_fd = nwy_uart_open("URT1", 115200, FC_NONE);
        return g_log_fd;
    }
    return fd;
}

int nwy_hal_uart_open(const char *port_name, uint32_t baudrate)
{
    if (!port_name) return -1;
    int fd = nwy_uart_open((char *)port_name, baudrate, FC_NONE);
    if (fd >= 0) {
        g_log_fd = fd; /* Default log output */
    }
    return fd;
}

int nwy_hal_uart_write(int fd, const uint8_t *data, uint32_t length)
{
    if (!data || length == 0) return -1;
    int target_fd = resolve_target_fd(fd);
    if (target_fd < 0) return -1;

    int res = -1;
    if (uart_lock(1000)) {
        res = nwy_uart_write(target_fd, (uint8_t *)data, length);
        uart_unlock();
    } else {
        res = nwy_uart_write(target_fd, (uint8_t *)data, length);
    }
    return res;
}

int nwy_hal_uart_read(int fd, uint8_t *buf, uint32_t max_length)
{
    if (!buf || max_length == 0) return -1;
    int target_fd = resolve_target_fd(fd);
    if (target_fd < 0) return -1;
    return nwy_uart_read(target_fd, buf, max_length);
}

bool nwy_hal_uart_close(int fd)
{
    if (fd < 0) return false;
    if (g_log_fd == fd) {
        g_log_fd = -1;
    }
    return (nwy_uart_close(fd) == 0);
}

bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb)
{
    if (!cb) return false;
    int target_fd = resolve_target_fd(fd);
    if (target_fd < 0) return false;
    nwy_error_e ret = nwy_uart_rx_cb_register(target_fd, (nwy_uart_rx_callback)cb);
    return (ret == NWY_SUCCESS);
}

int nwy_hal_uart_printf(int fd, const char *fmt, ...)
{
    if (!fmt) return -1;
    int target_fd = resolve_target_fd(fd);
    if (target_fd < 0) return -1;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) return 0;
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
        buf[len] = '\0';
    }

    int res = -1;
    if (uart_lock(1000)) {
        res = nwy_uart_write(target_fd, (uint8_t *)buf, (uint32_t)len);
        uart_unlock();
    } else {
        res = nwy_uart_write(target_fd, (uint8_t *)buf, (uint32_t)len);
    }
    return res;
}

void nwy_hal_uart_set_log_fd(int fd)
{
    g_log_fd = fd;
}

int nwy_hal_uart_get_log_fd(void)
{
    return g_log_fd;
}

void nwy_hal_uart_set_log_level(nwy_hal_log_level_t level)
{
    g_current_log_level = level;
}

nwy_hal_log_level_t nwy_hal_uart_get_log_level(void)
{
    return g_current_log_level;
}

void nwy_hal_uart_enable_timestamp(bool enable)
{
    g_log_timestamp = enable;
}

bool nwy_hal_uart_is_timestamp_enabled(void)
{
    return g_log_timestamp;
}

static void uart_log_format(nwy_hal_log_level_t level, char level_char, const char *tag, const char *fmt, va_list args)
{
    if (level < g_current_log_level) return;
    if (!fmt || !tag) return;

    int target_fd = resolve_target_fd(g_log_fd);
    if (target_fd < 0) return;

    char content_buf[512];
    int content_len = vsnprintf(content_buf, sizeof(content_buf), fmt, args);
    if (content_len <= 0) return;
    if (content_len >= (int)sizeof(content_buf)) {
        content_len = (int)sizeof(content_buf) - 1;
        content_buf[content_len] = '\0';
    }

    char log_buf[640];
    int total_len = 0;

    if (g_log_timestamp) {
        int64 uptime = nwy_uptime_get();
        uint32_t sec = (uint32_t)(uptime / 1000);
        uint32_t ms = (uint32_t)(uptime % 1000);
        total_len = snprintf(log_buf, sizeof(log_buf), "[%c][%05lu.%03lu][%s] %s\r\n",
                             level_char, (unsigned long)sec, (unsigned long)ms, tag, content_buf);
    } else {
        total_len = snprintf(log_buf, sizeof(log_buf), "[%c][%s] %s\r\n",
                             level_char, tag, content_buf);
    }

    if (total_len <= 0) return;
    if (total_len >= (int)sizeof(log_buf)) {
        total_len = (int)sizeof(log_buf) - 1;
        log_buf[total_len] = '\0';
    }

    if (uart_lock(1000)) {
        nwy_uart_write(target_fd, (uint8_t *)log_buf, (uint32_t)total_len);
        uart_unlock();
    } else {
        nwy_uart_write(target_fd, (uint8_t *)log_buf, (uint32_t)total_len);
    }
}

void nwy_hal_uart_log_v(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    uart_log_format(NWY_HAL_LOG_VERBOSE, 'V', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_d(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    uart_log_format(NWY_HAL_LOG_DEBUG, 'D', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_i(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    uart_log_format(NWY_HAL_LOG_INFO, 'I', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_w(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    uart_log_format(NWY_HAL_LOG_WARN, 'W', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_e(const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    uart_log_format(NWY_HAL_LOG_ERROR, 'E', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_hex(const char *tag, const char *desc, const uint8_t *data, uint32_t length)
{
    (void)tag; (void)desc; (void)data; (void)length;
}
