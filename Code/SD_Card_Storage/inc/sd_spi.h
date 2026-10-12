#ifndef __SD_SPI_H__
#define __SD_SPI_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard SD card types */
#define SD_CARD_TYPE_UNKNOWN    0x00
#define SD_CARD_TYPE_MMC        0x01
#define SD_CARD_TYPE_SDV1       0x02
#define SD_CARD_TYPE_SDV2_SC    0x04  /* Standard Capacity (byte addressed) */
#define SD_CARD_TYPE_SDV2_HC    0x08  /* High Capacity / SDXC (block addressed) */

/* Default Pin Assignments for Neoway N706B */
#define SD_DEFAULT_SPI_BUS      1        /* Maps to "SPI1" (SSP0) */
#define SD_DEFAULT_CS_GPIO      34       /* Module Pin 79 (SSP0_FRM -> GPIO 34) */
#define SD_DEFAULT_PWR_GPIO     0        /* Module Pin 83 (GPIO_00 -> Q10 Drain -> SD VCC) */
#define SD_DEFAULT_INIT_SPEED   400000   /* 400 kHz during identification */
#define SD_DEFAULT_WORK_SPEED   12000000 /* 12 MHz standard operational clock */

/* Card descriptor details parsed from CID and CSD */
typedef struct {
    uint8_t  card_type;
    uint32_t sector_count;
    uint32_t sector_size;
    uint64_t capacity_bytes;
    char     product_name[6];
    uint8_t  manufacturer_id;
    uint16_t oem_id;
    uint32_t serial_number;
    uint8_t  revision_major;
    uint8_t  revision_minor;
    uint16_t manufacturing_year;
    uint8_t  manufacturing_month;
} sd_card_info_t;

/**
 * @brief Initialize low-level SPI controller and CS GPIO pin.
 * @param spi_bus_num SPI bus number (1 for "SPI1")
 * @param cs_gpio GPIO number for Chip Select (34 for Pin 79)
 * @return 0 on success, negative error code on failure.
 */
int sd_spi_hw_init(int spi_bus_num, int cs_gpio);

/**
 * @brief Deinitialize SPI controller and release CS pin.
 */
void sd_spi_hw_deinit(void);

/**
 * @brief Execute the full SD Card SPI reset and initialization sequence
 *        (CMD0 -> CMD8 -> ACMD41 loop -> CMD58 OCR -> CSD/CID parse).
 * @return 0 on success, negative error code on failure.
 */
int sd_spi_card_init(void);

/**
 * @brief Read 512-byte sector(s) from SD card.
 * @param sector Starting sector number (LBA)
 * @param buffer Destination buffer
 * @param count Number of 512-byte sectors to read
 * @return 0 on success, negative error code on failure.
 */
int sd_spi_read_sector(uint32_t sector, uint8_t *buffer, uint32_t count);

/**
 * @brief Write 512-byte sector(s) to SD card.
 * @param sector Starting sector number (LBA)
 * @param buffer Source buffer
 * @param count Number of 512-byte sectors to write
 * @return 0 on success, negative error code on failure.
 */
int sd_spi_write_sector(uint32_t sector, const uint8_t *buffer, uint32_t count);

/**
 * @brief Wait until card finishes internal programming / busy state.
 * @return 0 on ready, negative error code on timeout.
 */
int sd_spi_sync(void);

/**
 * @brief Get parsed card information (capacity, product name, serial, etc.).
 * @return Pointer to sd_card_info_t or NULL if uninitialized.
 */
const sd_card_info_t *sd_spi_get_info(void);

/**
 * @brief Check if SD card is present, initialized, and ready for transfers.
 * @return true if ready, false otherwise.
 */
bool sd_spi_is_ready(void);

/**
 * @brief Mark SD card as not ready/uninitialized (called on I/O disconnect).
 */
void sd_spi_set_unready(void);

/**
 * @brief Ping card to quickly verify physical presence and responsiveness.
 * @return true if card is present and responsive, false otherwise.
 */
bool sd_spi_ping(void);

/**
 * @brief Thread-safe formatted logging to debug UART.
 */
void sd_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __SD_SPI_H__ */
