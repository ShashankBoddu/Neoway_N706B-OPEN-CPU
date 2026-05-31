#include "hal/uart/nwy_hal_uart.h"
#include "nwy_uart_api.h"

int nwy_hal_uart_open(const char *port_name, uint32_t baudrate) {
    if (!port_name) return -1;
    // N706B UART SDK opens with FC_NONE by default for standard tests
    return nwy_uart_open((char *)port_name, baudrate, FC_NONE);
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
    return (nwy_uart_close(fd) == 0);
}

bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb) {
    if (fd < 0 || !cb) return false;
    
    // Register callback via SDK function
    nwy_uart_rx_cb_register(fd, (void *)cb);
    return true;
}
