#include "hal/uart/nwy_hal_uart.h"
#include "nwy_uart_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NWY_CORE_UART_MODE_ADDR 0x7e22e3c0

int nwy_hal_uart_open(const char *port_name, uint32_t baudrate) {
    if (!port_name) return -1;

    // Critical Neoway OpenCPU Fix:
    // Core firmware cp.bin checks global variable uartMode at 0x7e22e3c0.
    // If uartMode == 0 (NWY_UART_MODE_AT, default uninitialized in BSS),
    // nwy_uart_open skips the hardware UART driver open function (0x14a854)
    // for non-debug ports and immediately returns without initializing hardware!
    // We explicitly set uartMode = 1 (NWY_UART_MODE_DATA) to ensure the hardware
    // transceiver registers and RX buffers are fully initialized.
    volatile uint8_t *p_uart_mode = (volatile uint8_t *)NWY_CORE_UART_MODE_ADDR;
    *p_uart_mode = NWY_UART_MODE_DATA;

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

int nwy_hal_uart_write(int fd, const uint8_t *data, uint32_t length) {
    if (fd < 0 || !data || length == 0) return -1;
    return nwy_uart_write(fd, data, length);
}

int nwy_hal_uart_printf(int fd, const char *fmt, ...) {
    if (fd < 0 || !fmt) return -1;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        return nwy_uart_write(fd, (const uint8_t *)buf, len);
    }
    return 0;
}

int nwy_hal_uart_read(int fd, uint8_t *buf, uint32_t max_length) {
    if (fd < 0 || !buf || max_length == 0) return -1;
    return nwy_uart_read(fd, buf, max_length);
}

bool nwy_hal_uart_close(int fd) {
    if (fd < 0) return false;
    return (nwy_uart_close(fd) == 0);
}

bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb) {
    if (fd < 0 || !cb) return false;
    
    // Register callback via SDK function
    nwy_uart_rx_cb_register(fd, (void *)cb);
    return true;
}
