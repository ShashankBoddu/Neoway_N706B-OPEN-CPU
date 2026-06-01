#include "hal/uart/nwy_hal_uart.h"
#include "nwy_uart_api.h"
#include <stdarg.h>
#include <stdio.h>

static int g_log_fd = -1;

int nwy_hal_uart_open(const char *port_name, uint32_t baudrate) {
    if (!port_name) return -1;
    int fd = nwy_uart_open((char *)port_name, baudrate, FC_NONE);
    if (fd >= 0) {
        g_log_fd = fd; // Set opened port as default log output
    }
    return fd;
}

int nwy_hal_uart_write(int fd, const uint8_t *data, uint32_t length) {
    if (fd < 0 || !data || length == 0) return -1;
    return nwy_uart_write(fd, data, length);
}

int nwy_hal_uart_read(int fd, uint8_t *buf, uint32_t max_length) {
    if (fd < 0 || !buf || max_length == 0) return -1;
    return nwy_uart_read(fd, buf, max_length);
}

bool nwy_hal_uart_close(int fd) {
    if (fd < 0) return false;
    if (g_log_fd == fd) {
        g_log_fd = -1;
    }
    return (nwy_uart_close(fd) == 0);
}

bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb) {
    if (fd < 0 || !cb) return false;
    nwy_error_e ret = nwy_uart_rx_cb_register(fd, (nwy_uart_rx_callback)cb);
    return (ret == NWY_SUCCESS);
}

int nwy_hal_uart_printf(int fd, const char *fmt, ...) {
    if (fd < 0 || !fmt) return -1;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        return nwy_uart_write(fd, (uint8_t *)buf, len);
    }
    return 0;
}

void nwy_hal_uart_set_log_fd(int fd) {
    g_log_fd = fd;
}

static void uart_log_format(char level_char, const char *tag, const char *fmt, va_list args) {
    if (g_log_fd < 0 || !fmt || !tag) return;
    char content_buf[256];
    int content_len = vsnprintf(content_buf, sizeof(content_buf), fmt, args);
    if (content_len <= 0) return;
    
    char log_buf[320];
    int total_len = snprintf(log_buf, sizeof(log_buf), "[%c][%s] %s\r\n", level_char, tag, content_buf);
    
    if (total_len > 0) {
        nwy_uart_write(g_log_fd, (uint8_t *)log_buf, total_len);
    }
}

void nwy_hal_uart_log_i(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    uart_log_format('I', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_w(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    uart_log_format('W', tag, fmt, args);
    va_end(args);
}

void nwy_hal_uart_log_e(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    uart_log_format('E', tag, fmt, args);
    va_end(args);
}
