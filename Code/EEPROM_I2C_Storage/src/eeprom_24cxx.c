#include "eeprom_24cxx.h"
#include "nwy_osi_api.h"
#include <string.h>
#include <stdio.h>

static eeprom_config_t s_cfg;
static eeprom_info_t   s_info;
static bool            s_initialized = false;

/* Standard IEEE 802.3 CRC-32 Lookup / Bitwise Engine */
uint32_t eeprom_crc32(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) return 0;

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/* Acknowledge Polling to wait for internal programming cycle */
static int eeprom_wait_ready(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (i2c_bus_ping(s_cfg.dev_addr)) {
            return 0; /* Chip responded with ACK -> Write cycle complete! */
        }
        nwy_thread_sleep(1); /* 1 ms yield */
        elapsed++;
    }
    return -1; /* Timeout */
}

int eeprom_init(const eeprom_config_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        s_cfg.dev_addr            = EEPROM_DEFAULT_DEV_ADDR;
        s_cfg.total_bytes         = EEPROM_TOTAL_BYTES;
        s_cfg.page_size           = EEPROM_PAGE_SIZE;
        s_cfg.verify_after_write  = true;
        s_cfg.ack_poll_timeout_ms = EEPROM_ACK_POLL_TIMEOUT_MS;
    }

    memset(&s_info, 0, sizeof(s_info));
    s_info.total_bytes = s_cfg.total_bytes;
    s_info.page_size   = s_cfg.page_size;
    s_info.page_count  = s_cfg.total_bytes / s_cfg.page_size;

    /* 1. Initialize low-level I2C bus subsystem */
    int ret = i2c_bus_init(NULL);
    if (ret != 0) {
        i2c_log("  [EEPROM ERROR] Low-level I2C bus initialization failed (%d)", ret);
        return -1;
    }

    /* 2. Ping EEPROM at address 0x50 */
    if (!eeprom_ping()) {
        i2c_log("  [EEPROM ERROR] Device not responding at address 0x%02X! Check power and wiring.", s_cfg.dev_addr);
        return -2;
    }

    /* 3. Try reading factory 128-bit serial number */
    if (eeprom_read_serial_number(s_info.serial_number) == 0) {
        s_info.serial_number_valid = true;
    }

    s_initialized = true;
    return 0;
}

void eeprom_deinit(void)
{
    i2c_bus_deinit();
    s_initialized = false;
}

bool eeprom_ping(void)
{
    return i2c_bus_ping(s_cfg.dev_addr);
}

int eeprom_read_byte(uint16_t mem_addr, uint8_t *val)
{
    if (val == NULL) return -1;
    return eeprom_read(mem_addr, val, 1);
}

int eeprom_write_byte(uint16_t mem_addr, uint8_t val)
{
    return eeprom_write(mem_addr, &val, 1);
}

int eeprom_read(uint16_t mem_addr, uint8_t *data, uint16_t len)
{
    if (!s_initialized || data == NULL || len == 0) return -1;
    if ((uint32_t)mem_addr + len > s_cfg.total_bytes) return -2; /* Address out of bounds */

    /* 16-bit word address: MSB first, then LSB */
    uint8_t addr_bytes[2];
    addr_bytes[0] = (uint8_t)(mem_addr >> 8);
    addr_bytes[1] = (uint8_t)(mem_addr & 0xFF);

    /* Combined Write-Read with Repeated START */
    int ret = i2c_bus_write_read(s_cfg.dev_addr, addr_bytes, 2, data, len);
    if (ret != 0) {
        return -3;
    }
    return 0;
}

int eeprom_write(uint16_t mem_addr, const uint8_t *data, uint16_t len)
{
    if (!s_initialized || data == NULL || len == 0) return -1;
    if ((uint32_t)mem_addr + len > s_cfg.total_bytes) return -2;

    uint16_t written = 0;
    uint8_t page_buf[2 + EEPROM_PAGE_SIZE];

    /* Split write requests across physical page boundaries */
    while (len > 0) {
        uint16_t page_offset = mem_addr % s_cfg.page_size;
        uint16_t chunk = s_cfg.page_size - page_offset;
        if (chunk > len) {
            chunk = len;
        }

        /* Build frame: [Addr MSB] [Addr LSB] [Data 0..chunk-1] */
        page_buf[0] = (uint8_t)(mem_addr >> 8);
        page_buf[1] = (uint8_t)(mem_addr & 0xFF);
        memcpy(&page_buf[2], &data[written], chunk);

        /* Transmit page chunk */
        int ret = i2c_bus_write(s_cfg.dev_addr, page_buf, 2 + chunk);
        if (ret != 0) {
            i2c_log("  [EEPROM ERROR] Write failed at address 0x%04X (chunk=%u, code=%d)",
                    mem_addr, chunk, ret);
            return -3;
        }

        /* Non-blocking Acknowledge Polling until write cycle finishes */
        if (eeprom_wait_ready(s_cfg.ack_poll_timeout_ms) != 0) {
            i2c_log("  [EEPROM ERROR] ACK polling timeout after writing page at 0x%04X!", mem_addr);
            return -4;
        }

        mem_addr += chunk;
        written += chunk;
        len -= chunk;
    }

    /* Optional verify-after-write */
    if (s_cfg.verify_after_write) {
        uint8_t verify_buf[64];
        uint16_t verify_addr = mem_addr - written;
        uint16_t rem = written;
        uint16_t off = 0;

        while (rem > 0) {
            uint16_t v_chunk = (rem > sizeof(verify_buf)) ? sizeof(verify_buf) : rem;
            if (eeprom_read(verify_addr + off, verify_buf, v_chunk) != 0) {
                return -5;
            }
            if (memcmp(verify_buf, &data[off], v_chunk) != 0) {
                i2c_log("  [EEPROM ERROR] Data verification failed at 0x%04X!", verify_addr + off);
                return -6;
            }
            off += v_chunk;
            rem -= v_chunk;
        }
    }

    return 0;
}

int eeprom_write_verify(uint16_t mem_addr, const uint8_t *data, uint16_t len)
{
    bool orig_verify = s_cfg.verify_after_write;
    s_cfg.verify_after_write = true;
    int ret = eeprom_write(mem_addr, data, len);
    s_cfg.verify_after_write = orig_verify;
    return ret;
}

int eeprom_fill(uint16_t start_addr, uint16_t len, uint8_t pattern)
{
    if ((uint32_t)start_addr + len > s_cfg.total_bytes) return -1;

    uint8_t buf[64];
    memset(buf, pattern, sizeof(buf));

    uint16_t rem = len;
    uint16_t cur_addr = start_addr;

    while (rem > 0) {
        uint16_t chunk = (rem > sizeof(buf)) ? sizeof(buf) : rem;
        int ret = eeprom_write(cur_addr, buf, chunk);
        if (ret != 0) return ret;
        cur_addr += chunk;
        rem -= chunk;
    }

    return 0;
}

int eeprom_read_serial_number(uint8_t sn_buf[16])
{
    if (sn_buf == NULL) return -1;

    /* P24C128D Factory Serial Number: Device Addr 0x58, Word Addr 0x0800 */
    uint8_t addr_bytes[2] = { 0x08, 0x00 };
    int ret = i2c_bus_write_read(EEPROM_SECURITY_DEV_ADDR, addr_bytes, 2, sn_buf, 16);
    if (ret != 0) {
        /* Fallback: try address 0x0080 */
        addr_bytes[0] = 0x00;
        addr_bytes[1] = 0x80;
        ret = i2c_bus_write_read(EEPROM_SECURITY_DEV_ADDR, addr_bytes, 2, sn_buf, 16);
    }
    return ret;
}

int eeprom_read_id_page(uint8_t id_buf[64])
{
    if (id_buf == NULL) return -1;

    /* P24C128D Identification Page: Device Addr 0x58, Word Addr 0x0000 */
    uint8_t addr_bytes[2] = { 0x00, 0x00 };
    return i2c_bus_write_read(EEPROM_SECURITY_DEV_ADDR, addr_bytes, 2, id_buf, 64);
}

int eeprom_write_config_record(uint16_t mem_addr, uint32_t magic,
                               const void *data, uint16_t len)
{
    if (data == NULL || len == 0) return -1;
    uint32_t total_record_size = sizeof(eeprom_record_header_t) + len;
    if ((uint32_t)mem_addr + total_record_size > s_cfg.total_bytes) return -2;

    eeprom_record_header_t hdr;
    hdr.magic       = magic;
    hdr.version     = 1;
    hdr.payload_len = len;
    hdr.crc32       = eeprom_crc32((const uint8_t *)data, len);

    /* 1. Write header */
    int ret = eeprom_write(mem_addr, (const uint8_t *)&hdr, sizeof(hdr));
    if (ret != 0) return ret;

    /* 2. Write payload */
    ret = eeprom_write(mem_addr + sizeof(hdr), (const uint8_t *)data, len);
    if (ret != 0) return ret;

    return 0;
}

int eeprom_read_config_record(uint16_t mem_addr, uint32_t magic,
                              void *data, uint16_t max_len, uint16_t *actual_len)
{
    if (data == NULL || max_len == 0) return -1;
    if ((uint32_t)mem_addr + sizeof(eeprom_record_header_t) > s_cfg.total_bytes) return -2;

    eeprom_record_header_t hdr;
    int ret = eeprom_read(mem_addr, (uint8_t *)&hdr, sizeof(hdr));
    if (ret != 0) return -3;

    if (hdr.magic != magic) {
        i2c_log("  [EEPROM CONFIG] Magic signature mismatch at 0x%04X (got 0x%08X, expected 0x%08X)",
                mem_addr, hdr.magic, magic);
        return -4; /* Uninitialized or wrong record type */
    }

    if (hdr.payload_len > max_len) {
        i2c_log("  [EEPROM CONFIG] Record payload length (%u) exceeds buffer (%u)",
                hdr.payload_len, max_len);
        return -5;
    }

    ret = eeprom_read(mem_addr + sizeof(hdr), (uint8_t *)data, hdr.payload_len);
    if (ret != 0) return -6;

    uint32_t calc_crc = eeprom_crc32((const uint8_t *)data, hdr.payload_len);
    if (calc_crc != hdr.crc32) {
        i2c_log("  [EEPROM CONFIG] CRC32 corrupted! Stored: 0x%08X, Calculated: 0x%08X",
                hdr.crc32, calc_crc);
        return -7; /* Corrupted data */
    }

    if (actual_len != NULL) {
        *actual_len = hdr.payload_len;
    }

    return 0;
}

const eeprom_info_t *eeprom_get_info(void)
{
    if (!s_initialized) return NULL;
    return &s_info;
}
