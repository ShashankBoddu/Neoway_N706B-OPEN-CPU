#ifndef __NWY_HAL_UART_H__
#define __NWY_HAL_UART_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nwy_hal_uart_rx_cb_t)(const char *str, uint32_t length);

int nwy_hal_uart_open(const char *port_name, uint32_t baudrate);
int nwy_hal_uart_write(int fd, const uint8_t *data, uint32_t length);
int nwy_hal_uart_read(int fd, uint8_t *buf, uint32_t max_length);
bool nwy_hal_uart_close(int fd);
bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb);

// Printf and Logger functions
int nwy_hal_uart_printf(int fd, const char *fmt, ...);
void nwy_hal_uart_set_log_fd(int fd);
void nwy_hal_uart_log_i(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_w(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_e(const char *tag, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_UART_H__
