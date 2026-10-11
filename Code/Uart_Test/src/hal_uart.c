#include "hal_uart.h"
#include "nwy_uart_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int hal_uart_open(const char *port_name, uint32_t baudrate) {
    if (!port_name) return -1;

    int fd = nwy_uart_open((char *)port_name, baudrate, FC_NONE);
    if (fd >= 0) {
        nwy_uartdcb_t dcb = {
            .baudrate = baudrate,
            .databits = 8,
            .stopbits = 1,
            .parity = PB_NONE,
            .flowctrl = FC_NONE
        };
        nwy_uart_dcb_set(fd, &dcb);
    }
    return fd;
}

int hal_uart_write(int fd, const uint8_t *data, uint32_t length) {
    if (fd < 0 || !data || length == 0) return -1;
    return nwy_uart_write(fd, (const unsigned char *)data, length);
}

int hal_uart_printf(int fd, const char *fmt, ...) {
    if (fd < 0 || !fmt) return -1;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        return nwy_uart_write(fd, (const unsigned char *)buf, len);
    }
    return 0;
}

bool hal_uart_close(int fd) {
    if (fd < 0) return false;
    return (nwy_uart_close(fd) == 0);
}

bool hal_uart_register_rx_cb(int fd, hal_uart_rx_cb_t cb) {
    if (fd < 0 || !cb) return false;
    return (nwy_uart_rx_cb_register(fd, (nwy_uart_rx_callback)cb) == 0);
}
