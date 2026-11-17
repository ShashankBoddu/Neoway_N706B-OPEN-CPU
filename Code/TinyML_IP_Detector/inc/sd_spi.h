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

int sd_spi_hw_init(int spi_bus_num, int cs_gpio);
void sd_spi_hw_deinit(void);
int sd_spi_card_init(void);
int sd_spi_read_sector(uint32_t sector, uint8_t *buffer, uint32_t count);
int sd_spi_write_sector(uint32_t sector, const uint8_t *buffer, uint32_t count);
int sd_spi_sync(void);
const sd_card_info_t *sd_spi_get_info(void);
bool sd_spi_is_ready(void);
void sd_spi_set_unready(void);
void sd_log(const char *fmt, ...);
bool sd_spi_ping(void);
void sd_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __SD_SPI_H__ */
