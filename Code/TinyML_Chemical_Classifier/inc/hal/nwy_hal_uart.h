#ifndef __NWY_HAL_UART_H__
#define __NWY_HAL_UART_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HAL Log severity levels (prefixed NWY_HAL_LOG_* to avoid conflict with SDK) */
typedef enum {
    NWY_HAL_LOG_VERBOSE = 0,
    NWY_HAL_LOG_DEBUG   = 1,
    NWY_HAL_LOG_INFO    = 2,
    NWY_HAL_LOG_WARN    = 3,
    NWY_HAL_LOG_ERROR   = 4,
    NWY_HAL_LOG_NONE    = 5
} nwy_hal_log_level_t;

typedef void (*nwy_hal_uart_rx_cb_t)(const char *str, uint32_t length);

/* Hardware UART Port Management */
int nwy_hal_uart_open(const char *port_name, uint32_t baudrate);
int nwy_hal_uart_write(int fd, const uint8_t *data, uint32_t length);
int nwy_hal_uart_read(int fd, uint8_t *buf, uint32_t max_length);
bool nwy_hal_uart_close(int fd);
bool nwy_hal_uart_register_rx_cb(int fd, nwy_hal_uart_rx_cb_t cb);

/* Printf and Logger functions */
int nwy_hal_uart_printf(int fd, const char *fmt, ...);
void nwy_hal_uart_set_log_fd(int fd);
int nwy_hal_uart_get_log_fd(void);

/* Convenience macro for printf to default debug UART */
#define nwy_printf(fmt, ...) nwy_hal_uart_printf(0, fmt, ##__VA_ARGS__)

/* Log level filtering & uptime timestamp controls */
void nwy_hal_uart_set_log_level(nwy_hal_log_level_t level);
nwy_hal_log_level_t nwy_hal_uart_get_log_level(void);
void nwy_hal_uart_enable_timestamp(bool enable);
bool nwy_hal_uart_is_timestamp_enabled(void);

/* Structured Level Logging */
void nwy_hal_uart_log_v(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_d(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_i(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_w(const char *tag, const char *fmt, ...);
void nwy_hal_uart_log_e(const char *tag, const char *fmt, ...);

/* Hex dump logging utility for raw buffers / packets / crypto */
void nwy_hal_uart_log_hex(const char *tag, const char *desc, const uint8_t *data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_UART_H__
