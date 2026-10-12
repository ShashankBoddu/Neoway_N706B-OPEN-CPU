#include "i2c_bus.h"
#include "nwy_i2c_api.h"
#include "nwy_gpio_api.h"
#include "nwy_osi_api.h"
#include "nwy_uart_api.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define DEBUG_UART_PORT "URT1"

/* Module State */
static int                 s_hw_i2c_fd = -1;
static i2c_bus_config_t    s_cfg;
static bool                s_is_initialized = false;
static i2c_driver_mode_t   s_active_engine = I2C_MODE_SW;

/* Recursive Mutex Context */
static nwy_osi_mutex_t     s_bus_mutex = NULL;
static nwy_osi_thread_t    s_mutex_owner = NULL;
static uint32_t            s_recursion_count = 0;

/* Debug UART log handle */
static int s_log_uart_fd = -1;

void i2c_log(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    if (len > 0) {
        if (buf[len - 1] != '\n') {
            buf[len++] = '\r';
            buf[len++] = '\n';
            buf[len] = '\0';
        }
        if (s_log_uart_fd < 0) {
            s_log_uart_fd = nwy_uart_open(DEBUG_UART_PORT, 115200, FC_NONE);
        }
        if (s_log_uart_fd >= 0) {
            nwy_uart_write(s_log_uart_fd, (unsigned char *)buf, (unsigned int)len);
        }
    }
}

/* =========================================================================
 * MUTEX MANAGEMENT
 * ========================================================================= */
bool i2c_bus_lock(uint32_t timeout_ms)
{
    if (s_bus_mutex == NULL) return false;

    nwy_osi_thread_t cur = nwy_thread_get_current();
    if (s_mutex_owner == cur) {
        s_recursion_count++;
        return true;
    }

    nwy_error_e err = nwy_sdk_mutex_lock(s_bus_mutex, (int)timeout_ms);
    if (err == NWY_SUCCESS) {
        s_mutex_owner = cur;
        s_recursion_count = 1;
        return true;
    }
    return false;
}

void i2c_bus_unlock(void)
{
    if (s_bus_mutex == NULL) return;

    nwy_osi_thread_t cur = nwy_thread_get_current();
    if (s_mutex_owner == cur) {
        if (s_recursion_count > 1) {
            s_recursion_count--;
        } else {
            s_recursion_count = 0;
            s_mutex_owner = NULL;
            nwy_sdk_mutex_unlock(s_bus_mutex);
        }
    }
}

/* =========================================================================
 * LOW-LEVEL SOFTWARE BIT-BANG ENGINE
 * ========================================================================= */
static inline void bb_delay(void)
{
    /* Tight CPU assembly cycle loop for microsecond timing without RTOS context switch.
     * On Cortex-R5 running at ~300-600 MHz:
     * - 150 iterations gives ~1.25 µs half-cycle (400 kHz Fast Mode)
     * - 600 iterations gives ~5.0 µs half-cycle (100 kHz Standard Mode)
     */
    volatile uint32_t count = (s_cfg.speed_hz <= 100000) ? 600 : 150;
    while (count--) {
        __asm__ volatile("nop");
    }
}

static inline void bb_scl_high(void)
{
    /* Pull SCL high via open-drain (input with external pull-up) or output HIGH */
    nwy_gpio_direction_set(s_cfg.scl_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(s_cfg.scl_gpio, PIN_LEVEL_HIGH);
}

static inline void bb_scl_low(void)
{
    nwy_gpio_direction_set(s_cfg.scl_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(s_cfg.scl_gpio, PIN_LEVEL_LOW);
}

static inline void bb_sda_high(void)
{
    /* Float SDA high (input with pull-up) */
    nwy_gpio_direction_set(s_cfg.sda_gpio, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(s_cfg.sda_gpio, PIN_PULL_PU);
}

static inline void bb_sda_low(void)
{
    /* Drive SDA low */
    nwy_gpio_direction_set(s_cfg.sda_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(s_cfg.sda_gpio, PIN_LEVEL_LOW);
}

static inline int bb_sda_read(void)
{
    nwy_gpio_direction_set(s_cfg.sda_gpio, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(s_cfg.sda_gpio, PIN_PULL_PU);
    return nwy_gpio_value_get(s_cfg.sda_gpio);
}

static void bb_start(void)
{
    bb_sda_high();
    bb_scl_high();
    bb_delay();
    bb_sda_low();
    bb_delay();
    bb_scl_low();
    bb_delay();
}

static void bb_stop(void)
{
    bb_sda_low();
    bb_delay();
    bb_scl_high();
    bb_delay();
    bb_sda_high();
    bb_delay();
}

static bool bb_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1 << i)) {
            bb_sda_high();
        } else {
            bb_sda_low();
        }
        bb_delay();
        bb_scl_high();
        bb_delay();
        bb_scl_low();
    }

    /* Read ACK */
    bb_sda_high();
    bb_delay();
    bb_scl_high();
    bb_delay();
    int ack = (bb_sda_read() == 0); /* Slave drives SDA LOW for ACK */
    bb_scl_low();
    bb_delay();

    return (ack != 0);
}

static uint8_t bb_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    bb_sda_high();

    for (int i = 7; i >= 0; i--) {
        bb_delay();
        bb_scl_high();
        bb_delay();
        if (bb_sda_read()) {
            byte |= (1 << i);
        }
        bb_scl_low();
    }

    /* Send ACK or NACK */
    if (send_ack) {
        bb_sda_low();  /* ACK */
    } else {
        bb_sda_high(); /* NACK */
    }
    bb_delay();
    bb_scl_high();
    bb_delay();
    bb_scl_low();
    bb_sda_high();
    bb_delay();

    return byte;
}

/* =========================================================================
 * 9-CLOCK BUS RECOVERY SEQUENCE
 * ========================================================================= */
int i2c_bus_recover(uint32_t scl_gpio, uint32_t sda_gpio)
{
    /* 1. Configure SCL as output HIGH, SDA as input with pullup */
    nwy_gpio_direction_set(scl_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_pull_set(scl_gpio, PIN_PULL_PU);
    nwy_gpio_value_set(scl_gpio, PIN_LEVEL_HIGH);

    nwy_gpio_direction_set(sda_gpio, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(sda_gpio, PIN_PULL_PU);

    nwy_thread_usleep(10);

    /* If SDA is already HIGH, bus is healthy */
    if (nwy_gpio_value_get(sda_gpio) != 0) {
        /* Generate a clean STOP condition to reset slave state machine */
        nwy_gpio_direction_set(sda_gpio, PIN_DIRECTION_OUT);
        nwy_gpio_value_set(sda_gpio, PIN_LEVEL_LOW);
        nwy_thread_usleep(5);
        nwy_gpio_value_set(scl_gpio, PIN_LEVEL_HIGH);
        nwy_thread_usleep(5);
        nwy_gpio_value_set(sda_gpio, PIN_LEVEL_HIGH);
        nwy_thread_usleep(5);
        nwy_gpio_direction_set(sda_gpio, PIN_DIRECTION_IN);
        return 0;
    }

    i2c_log("  [I2C RECOVERY] SDA is STUCK LOW! Clocking 9 SCL cycles to free slave...");

    /* Clock SCL up to 9 times until SDA is released */
    bool freed = false;
    for (int i = 0; i < 9; i++) {
        nwy_gpio_value_set(scl_gpio, PIN_LEVEL_LOW);
        nwy_thread_usleep(5);
        nwy_gpio_value_set(scl_gpio, PIN_LEVEL_HIGH);
        nwy_thread_usleep(5);

        if (nwy_gpio_value_get(sda_gpio) != 0) {
            freed = true;
            break;
        }
    }

    /* Send STOP condition */
    nwy_gpio_direction_set(sda_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(sda_gpio, PIN_LEVEL_LOW);
    nwy_thread_usleep(5);
    nwy_gpio_value_set(scl_gpio, PIN_LEVEL_HIGH);
    nwy_thread_usleep(5);
    nwy_gpio_value_set(sda_gpio, PIN_LEVEL_HIGH);
    nwy_thread_usleep(5);
    nwy_gpio_direction_set(sda_gpio, PIN_DIRECTION_IN);

    if (freed && nwy_gpio_value_get(sda_gpio) != 0) {
        i2c_log("  [I2C RECOVERY] Bus freed successfully! SDA is now HIGH.");
        return 0;
    } else {
        i2c_log("  [I2C ERROR] SDA line remains STUCK LOW after 9 clocks! Check pull-ups or hardware fault.");
        return -1;
    }
}

/* =========================================================================
 * HARDWARE I2C ENGINE
 * ========================================================================= */
static bool hw_i2c_init_controller(void)
{
    nwy_i2c_mode_e mode = (s_cfg.speed_hz > 100000) ? NWY_I2C_BPS_400K : NWY_I2C_BPS_100K;
    s_hw_i2c_fd = nwy_i2c_init(s_cfg.bus_name, mode);
    if (s_hw_i2c_fd < 0) {
        return false;
    }
    return true;
}

/* =========================================================================
 * PUBLIC BUS API IMPLEMENTATION
 * ========================================================================= */
int i2c_bus_init(const i2c_bus_config_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        s_cfg.bus_name   = I2C2_BUS_NAME;
        s_cfg.speed_hz   = I2C_DEFAULT_SPEED_HZ;
        s_cfg.scl_gpio   = I2C2_DEFAULT_SCL_GPIO;
        s_cfg.sda_gpio   = I2C2_DEFAULT_SDA_GPIO;
        s_cfg.mode       = I2C_MODE_AUTO;
        s_cfg.timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    }

    /* 1. Create Recursive Mutex */
    if (s_bus_mutex == NULL) {
        nwy_sdk_mutex_create(&s_bus_mutex);
        s_mutex_owner = NULL;
        s_recursion_count = 0;
    }

    if (!i2c_bus_lock(1000)) return -1;

    /* 2. Execute 9-Clock Bus Recovery on Startup */
    i2c_bus_recover(s_cfg.scl_gpio, s_cfg.sda_gpio);

    /* 3. Determine Engine (Hardware or Software Bit-Bang) */
    s_active_engine = I2C_MODE_SW; /* Default safe engine */

    if (s_cfg.mode == I2C_MODE_HW || s_cfg.mode == I2C_MODE_AUTO) {
        if (hw_i2c_init_controller()) {
            s_active_engine = I2C_MODE_HW;
        } else {
            i2c_log("  [I2C NOTE] Hardware I2C init returned %d, falling back to Soft-I2C Engine.", s_hw_i2c_fd);
            s_active_engine = I2C_MODE_SW;
        }
    }

    s_is_initialized = true;
    i2c_bus_unlock();

    return 0;
}

void i2c_bus_deinit(void)
{
    if (!i2c_bus_lock(1000)) return;

    if (s_hw_i2c_fd >= 0) {
        nwy_i2c_deinit(s_hw_i2c_fd);
        s_hw_i2c_fd = -1;
    }

    /* Float bus lines with pull-up */
    nwy_gpio_direction_set(s_cfg.scl_gpio, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(s_cfg.scl_gpio, PIN_PULL_PU);
    nwy_gpio_direction_set(s_cfg.sda_gpio, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(s_cfg.sda_gpio, PIN_PULL_PU);

    s_is_initialized = false;
    i2c_bus_unlock();
}

bool i2c_bus_ping(uint8_t dev_addr_7bit)
{
    if (!i2c_bus_lock(500)) return false;

    bool ack = false;

    if (s_active_engine == I2C_MODE_HW && s_hw_i2c_fd >= 0) {
        uint8_t dummy = 0;
        /* Use raw put byte with START + STOP */
        nwy_error_e err = nwy_i2c_raw_put_byte(s_hw_i2c_fd, (dev_addr_7bit << 1) | 0, 1, 1);
        if (err == NWY_SUCCESS) {
            ack = true;
        } else {
            /* Retry via bit-bang probe */
            bb_start();
            ack = bb_write_byte((dev_addr_7bit << 1) | 0);
            bb_stop();
        }
    } else {
        bb_start();
        ack = bb_write_byte((dev_addr_7bit << 1) | 0);
        bb_stop();
    }

    i2c_bus_unlock();
    return ack;
}

int i2c_bus_write(uint8_t dev_addr_7bit, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return -1;
    if (!i2c_bus_lock(s_cfg.timeout_ms)) return -2;

    int ret = 0;

    if (s_active_engine == I2C_MODE_HW && s_hw_i2c_fd >= 0) {
        /* Send START + SlaveAddr */
        nwy_error_e err = nwy_i2c_raw_put_byte(s_hw_i2c_fd, (dev_addr_7bit << 1) | 0, 1, (len == 0) ? 1 : 0);
        if (err != NWY_SUCCESS) {
            ret = -3;
        } else {
            for (uint16_t i = 0; i < len; i++) {
                int stop = (i == (len - 1)) ? 1 : 0;
                err = nwy_i2c_raw_put_byte(s_hw_i2c_fd, data[i], 0, stop);
                if (err != NWY_SUCCESS) {
                    ret = -4;
                    break;
                }
            }
        }

        /* If HW engine failed, fallback to software bit-bang seamlessly */
        if (ret != 0 && s_cfg.mode == I2C_MODE_AUTO) {
            bb_start();
            if (!bb_write_byte((dev_addr_7bit << 1) | 0)) {
                bb_stop();
                ret = -5;
            } else {
                ret = 0;
                for (uint16_t i = 0; i < len; i++) {
                    if (!bb_write_byte(data[i])) {
                        ret = -6;
                        break;
                    }
                }
                bb_stop();
            }
        }
    } else {
        bb_start();
        if (!bb_write_byte((dev_addr_7bit << 1) | 0)) {
            bb_stop();
            ret = -5;
        } else {
            for (uint16_t i = 0; i < len; i++) {
                if (!bb_write_byte(data[i])) {
                    ret = -6;
                    break;
                }
            }
            bb_stop();
        }
    }

    i2c_bus_unlock();
    return ret;
}

int i2c_bus_read(uint8_t dev_addr_7bit, uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return -1;
    if (!i2c_bus_lock(s_cfg.timeout_ms)) return -2;

    int ret = 0;

    if (s_active_engine == I2C_MODE_HW && s_hw_i2c_fd >= 0) {
        /* Send START + SlaveAddr (Read) */
        nwy_error_e err = nwy_i2c_raw_put_byte(s_hw_i2c_fd, (dev_addr_7bit << 1) | 1, 1, 0);
        if (err != NWY_SUCCESS) {
            ret = -3;
        } else {
            for (uint16_t i = 0; i < len; i++) {
                int stop = (i == (len - 1)) ? 1 : 0;
                err = nwy_i2c_raw_get_byte(s_hw_i2c_fd, &data[i], 0, stop);
                if (err != NWY_SUCCESS) {
                    ret = -4;
                    break;
                }
            }
        }

        if (ret != 0 && s_cfg.mode == I2C_MODE_AUTO) {
            bb_start();
            if (!bb_write_byte((dev_addr_7bit << 1) | 1)) {
                bb_stop();
                ret = -5;
            } else {
                ret = 0;
                for (uint16_t i = 0; i < len; i++) {
                    bool send_ack = (i < (len - 1));
                    data[i] = bb_read_byte(send_ack);
                }
                bb_stop();
            }
        }
    } else {
        bb_start();
        if (!bb_write_byte((dev_addr_7bit << 1) | 1)) {
            bb_stop();
            ret = -5;
        } else {
            for (uint16_t i = 0; i < len; i++) {
                bool send_ack = (i < (len - 1));
                data[i] = bb_read_byte(send_ack);
            }
            bb_stop();
        }
    }

    i2c_bus_unlock();
    return ret;
}

int i2c_bus_write_read(uint8_t dev_addr_7bit,
                       const uint8_t *tx_data, uint16_t tx_len,
                       uint8_t *rx_data, uint16_t rx_len)
{
    if (rx_data == NULL || rx_len == 0) return -1;
    if (!i2c_bus_lock(s_cfg.timeout_ms)) return -2;

    int ret = 0;

    /* Always execute using deterministic repeated-start bit-bang engine for 100% address integrity */
    bb_start();
    if (tx_data != NULL && tx_len > 0) {
        if (!bb_write_byte((dev_addr_7bit << 1) | 0)) {
            bb_stop();
            i2c_bus_unlock();
            return -3;
        }
        for (uint16_t i = 0; i < tx_len; i++) {
            if (!bb_write_byte(tx_data[i])) {
                bb_stop();
                i2c_bus_unlock();
                return -4;
            }
        }
    }

    /* Repeated START */
    bb_start();
    if (!bb_write_byte((dev_addr_7bit << 1) | 1)) {
        bb_stop();
        i2c_bus_unlock();
        return -5;
    }

    for (uint16_t i = 0; i < rx_len; i++) {
        bool send_ack = (i < (rx_len - 1));
        rx_data[i] = bb_read_byte(send_ack);
    }
    bb_stop();

    i2c_bus_unlock();
    return ret;
}

int i2c_bus_scan(void)
{
    int count = 0;
    i2c_log("--- Scanning I2C Bus (%s: SCL=P%u, SDA=P%u) ---",
            s_cfg.bus_name, s_cfg.scl_gpio, s_cfg.sda_gpio);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_bus_ping(addr)) {
            i2c_log("  -> Found I2C Device at Address: 0x%02X (%u)", addr, addr);
            count++;
        }
    }

    if (count == 0) {
        i2c_log("  -> No I2C devices responded on the bus.");
    } else {
        i2c_log("--- Scan complete: %d device(s) found ---", count);
    }

    return count;
}
