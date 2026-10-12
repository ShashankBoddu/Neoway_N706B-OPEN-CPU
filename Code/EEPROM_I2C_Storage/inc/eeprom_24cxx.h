#ifndef __EEPROM_24CXX_H__
#define __EEPROM_24CXX_H__

#include <stdint.h>
#include <stdbool.h>
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* P24C128D / AT24C128 Hardware Constants */
#define EEPROM_DEFAULT_DEV_ADDR     0x50     /* 7-bit I2C Slave Address (E0=E1=E2=GND) */
#define EEPROM_SECURITY_DEV_ADDR    0x58     /* 7-bit Device Address for ID Page / SN (1011b) */
#define EEPROM_TOTAL_BYTES          16384    /* 128 Kbit = 16,384 Bytes (0x0000 - 0x3FFF) */
#define EEPROM_PAGE_SIZE            64       /* 64 Bytes per page */
#define EEPROM_TOTAL_PAGES          (EEPROM_TOTAL_BYTES / EEPROM_PAGE_SIZE) /* 256 pages */
#define EEPROM_MAX_WRITE_TIME_MS    5        /* 5 ms maximum internal write cycle time */
#define EEPROM_ACK_POLL_TIMEOUT_MS  10       /* 10 ms bounded ACK polling timeout */

/* EEPROM Descriptor Configuration */
typedef struct {
    uint8_t   dev_addr;           /* 7-bit I2C address (default: 0x50) */
    uint16_t  total_bytes;        /* Total capacity (default: 16384) */
    uint16_t  page_size;          /* Physical page size (default: 64) */
    bool      verify_after_write; /* Verify bytes after every write */
    uint32_t  ack_poll_timeout_ms;/* ACK polling timeout in ms (default: 10) */
} eeprom_config_t;

/* Chip Information & Identification Descriptor */
typedef struct {
    uint16_t  total_bytes;
    uint16_t  page_size;
    uint16_t  page_count;
    uint8_t   serial_number[16];   /* P24C128D 128-bit factory unique serial number */
    bool      serial_number_valid;
} eeprom_info_t;

/* Structured Configuration Block Header (for crash-proof settings storage) */
typedef struct {
    uint32_t  magic;               /* 4-byte Magic signature (e.g. 0x4E575931 "NWY1") */
    uint16_t  version;             /* Schema version */
    uint16_t  payload_len;         /* Actual length of stored user data struct */
    uint32_t  crc32;               /* CRC-32 (IEEE 802.3) over payload data */
} __attribute__((packed)) eeprom_record_header_t;

/**
 * @brief Initialize EEPROM subsystem and verify presence on the I2C bus.
 * @param cfg Pointer to configuration struct, or NULL for P24C128D defaults.
 * @return 0 on success, negative error code on failure.
 */
int eeprom_init(const eeprom_config_t *cfg);

/**
 * @brief Deinitialize EEPROM driver.
 */
void eeprom_deinit(void);

/**
 * @brief Ping EEPROM device to verify responsiveness.
 * @return true if device responds with ACK, false if absent or busy.
 */
bool eeprom_ping(void);

/**
 * @brief Read single byte from specified EEPROM memory address.
 * @param mem_addr Memory address (0x0000 to 0x3FFF)
 * @param val Destination byte pointer
 * @return 0 on success, negative error code on failure.
 */
int eeprom_read_byte(uint16_t mem_addr, uint8_t *val);

/**
 * @brief Write single byte to specified EEPROM memory address with ACK polling.
 * @param mem_addr Memory address (0x0000 to 0x3FFF)
 * @param val Data byte to write
 * @return 0 on success, negative error code on failure.
 */
int eeprom_write_byte(uint16_t mem_addr, uint8_t val);

/**
 * @brief Sequential read of arbitrary length from EEPROM memory.
 *        Reads transparently across page boundaries up to end of memory.
 * @param mem_addr Starting memory address (0x0000 to 0x3FFF)
 * @param data Destination buffer
 * @param len Number of bytes to read
 * @return 0 on success, negative error code on failure.
 */
int eeprom_read(uint16_t mem_addr, uint8_t *data, uint16_t len);

/**
 * @brief Industrial page-aware write of arbitrary length to EEPROM.
 *        Automatically splits transfers at 64-byte page boundaries to
 *        prevent internal address roll-over corruption, with ACK polling.
 * @param mem_addr Starting memory address (0x0000 to 0x3FFF)
 * @param data Source buffer to write
 * @param len Number of bytes to write
 * @return 0 on success, negative error code on failure.
 */
int eeprom_write(uint16_t mem_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Write data and immediately read back to verify 100% byte integrity.
 * @param mem_addr Starting memory address
 * @param data Source buffer
 * @param len Number of bytes
 * @return 0 on success and verified match, negative error code on failure/mismatch.
 */
int eeprom_write_verify(uint16_t mem_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Erase / Format entire EEPROM or section with specified pattern (e.g. 0xFF).
 * @param start_addr Starting address
 * @param len Number of bytes to fill
 * @param pattern Fill byte (typically 0xFF)
 * @return 0 on success, negative error code on failure.
 */
int eeprom_fill(uint16_t start_addr, uint16_t len, uint8_t pattern);

/**
 * @brief Read P24C128D Factory Unique 128-bit (16 bytes) Serial Number.
 * @param sn_buf Buffer of at least 16 bytes
 * @return 0 on success, negative error code on failure.
 */
int eeprom_read_serial_number(uint8_t sn_buf[16]);

/**
 * @brief Read P24C128D 64-byte Identification Page.
 * @param id_buf Buffer of at least 64 bytes
 * @return 0 on success, negative error code on failure.
 */
int eeprom_read_id_page(uint8_t id_buf[64]);

/**
 * @brief Store arbitrary configuration structure with CRC32 integrity check.
 *        Wraps data in a self-validating record header.
 * @param mem_addr Target memory address
 * @param magic Unique 32-bit Magic ID to identify the configuration schema
 * @param data Pointer to configuration struct
 * @param len Size of configuration struct in bytes
 * @return 0 on success and verified write, negative error code on failure.
 */
int eeprom_write_config_record(uint16_t mem_addr, uint32_t magic,
                               const void *data, uint16_t len);

/**
 * @brief Read and validate configuration structure using CRC32 integrity check.
 * @param mem_addr Memory address where record was stored
 * @param magic Expected 32-bit Magic ID
 * @param data Destination buffer for configuration struct
 * @param max_len Maximum buffer size
 * @param actual_len Optional output pointer for actual payload length
 * @return 0 on success with valid CRC32, negative error code if corrupted or mismatch.
 */
int eeprom_read_config_record(uint16_t mem_addr, uint32_t magic,
                              void *data, uint16_t max_len, uint16_t *actual_len);

/**
 * @brief Compute standard IEEE 802.3 CRC-32 checksum.
 * @param data Input buffer
 * @param len Buffer length
 * @return 32-bit CRC value.
 */
uint32_t eeprom_crc32(const uint8_t *data, size_t len);

/**
 * @brief Get active EEPROM configuration and parsed metadata.
 * @return Pointer to eeprom_info_t or NULL if uninitialized.
 */
const eeprom_info_t *eeprom_get_info(void);

#ifdef __cplusplus
}
#endif

#endif /* __EEPROM_24CXX_H__ */
