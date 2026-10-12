#include "sd_spi.h"
#include "nwy_spi_api.h"
#include "nwy_gpio_api.h"
#include "nwy_osi_api.h"
#include <string.h>
#include <stdio.h>

#define SD_CMD_TIMEOUT_COUNT    200
#define SD_BUSY_TIMEOUT_MS      1000

/* SD SPI Command Definitions */
#define CMD0    0   /* GO_IDLE_STATE */
#define CMD1    1   /* SEND_OP_COND */
#define CMD8    8   /* SEND_IF_COND */
#define CMD9    9   /* SEND_CSD */
#define CMD10   10  /* SEND_CID */
#define CMD12   12  /* STOP_TRANSMISSION */
#define CMD16   16  /* SET_BLOCKLEN */
#define CMD17   17  /* READ_SINGLE_BLOCK */
#define CMD18   18  /* READ_MULTIPLE_BLOCK */
#define CMD24   24  /* WRITE_BLOCK */
#define CMD25   25  /* WRITE_MULTIPLE_BLOCK */
#define CMD55   55  /* APP_CMD */
#define CMD58   58  /* READ_OCR */
#define ACMD41  41  /* SD_SEND_OP_COND (preceded by CMD55) */

/* Data Tokens */
#define TOKEN_START_BLOCK   0xFE
#define TOKEN_DATA_ACCEPTED 0x05

/* Module state variables */
static int             s_spi_hd = -1;
static int             s_spi_bus = SD_DEFAULT_SPI_BUS;
static int             s_cs_gpio = SD_DEFAULT_CS_GPIO;
static uint32_t        s_cur_speed = 0;
static bool            s_card_ready = false;
static sd_card_info_t  s_card_info;

/* DMA-aligned bounce buffers for Crane ASR1605 hardware SPI controller */
static uint8_t s_dma_tx[512 + 16] __attribute__((aligned(16)));
static uint8_t s_dma_rx[512 + 16] __attribute__((aligned(16)));
static uint8_t s_cmd_tx[16]       __attribute__((aligned(16)));
static uint8_t s_cmd_rx[16]       __attribute__((aligned(16)));

static void sd_delay_ms(uint32_t ms)
{
    if (ms >= 5) {
        nwy_thread_sleep(ms);
    } else {
        nwy_thread_usleep(ms * 1000);
    }
}

static inline void sd_cs_low(void)
{
    nwy_gpio_value_set((uint32_t)s_cs_gpio, PIN_LEVEL_LOW);
}

static uint8_t sd_spi_transfer_byte(uint8_t byte)
{
    uint8_t tx = byte;
    uint8_t rx = 0xFF;
    s_cmd_tx[0] = tx;
    s_cmd_rx[0] = 0xFF;
    if (s_spi_hd >= 0) {
        nwy_spi_transfer(s_spi_hd, SPI_CS_0, s_cmd_tx, s_cmd_rx, 1);
        rx = s_cmd_rx[0];
    }
    return rx;
}

static inline uint8_t sd_spi_rx_byte(void)
{
    return sd_spi_transfer_byte(0xFF);
}

static inline void sd_cs_high(void)
{
    nwy_gpio_value_set((uint32_t)s_cs_gpio, PIN_LEVEL_HIGH);
    /* Clock out 1 dummy byte with CS high to release SD MISO line */
    sd_spi_rx_byte();
}

static int sd_spi_wait_ready(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (sd_spi_rx_byte() == 0xFF) {
            return 0;
        }
        sd_delay_ms(1);
        elapsed++;
    }
    return -1;
}

static int sd_spi_set_speed(uint32_t speed_hz)
{
    if (s_spi_hd >= 0 && s_cur_speed == speed_hz) {
        return 0; /* Already running at this clock rate */
    }
    char *bus_name = (s_spi_bus == 2) ? NAME_SPI_BUS_2 : NAME_SPI_BUS_1;
    if (s_spi_hd >= 0) {
        nwy_gpio_value_set((uint32_t)s_cs_gpio, PIN_LEVEL_HIGH);
        nwy_spi_deinit(s_spi_hd);
        s_spi_hd = -1;
        s_cur_speed = 0;
        nwy_thread_sleep(10); /* 10 ms FreeRTOS yield hardware settling */
    }
    s_spi_hd = nwy_spi_init(bus_name, SPI_MODE_0, speed_hz, 8);
    if (s_spi_hd < 0) {
        return s_spi_hd;
    }
    s_cur_speed = speed_hz;
    return 0;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    uint8_t resp = 0xFF;
    int timeout = 0;

    /* Wait for card ready before command (skip for CMD0) */
    if (cmd != CMD0) {
        sd_spi_wait_ready(50);
    }

    /* Construct 6-byte command frame */
    s_cmd_tx[0] = (uint8_t)(0x40 | cmd);
    s_cmd_tx[1] = (uint8_t)(arg >> 24);
    s_cmd_tx[2] = (uint8_t)(arg >> 16);
    s_cmd_tx[3] = (uint8_t)(arg >> 8);
    s_cmd_tx[4] = (uint8_t)(arg);
    s_cmd_tx[5] = crc;

    nwy_spi_transfer(s_spi_hd, SPI_CS_0, s_cmd_tx, s_cmd_rx, 6);

    /* Wait for valid R1 response (MSB must be 0) */
    while (timeout++ < SD_CMD_TIMEOUT_COUNT) {
        resp = sd_spi_rx_byte();
        if ((resp & 0x80) == 0) {
            break;
        }
    }
    return resp;
}

int sd_spi_hw_init(int spi_bus_num, int cs_gpio)
{
    s_spi_bus = (spi_bus_num > 0) ? spi_bus_num : SD_DEFAULT_SPI_BUS;
    s_cs_gpio = (cs_gpio >= 0) ? cs_gpio : SD_DEFAULT_CS_GPIO;
    s_card_ready = false;
    s_cur_speed = 0;
    memset(&s_card_info, 0, sizeof(s_card_info));

    /* 1. Ensure SD Card VCC rail is powered ON via GPIO 0 (Q10 Drain -> SD VCC) */
    nwy_gpio_direction_set((uint32_t)SD_DEFAULT_PWR_GPIO, PIN_DIRECTION_OUT);
    nwy_gpio_pull_set((uint32_t)SD_DEFAULT_PWR_GPIO, PIN_PULL_PU);
    nwy_gpio_value_set((uint32_t)SD_DEFAULT_PWR_GPIO, PIN_LEVEL_HIGH);
    nwy_thread_sleep(50); /* 50 ms VCC power-on ramp */

    /* 2. Configure Pin 79 (GPIO 34) as Output, Idle HIGH */
    nwy_gpio_direction_set((uint32_t)s_cs_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_pull_set((uint32_t)s_cs_gpio, PIN_PULL_PU);
    nwy_gpio_value_set((uint32_t)s_cs_gpio, PIN_LEVEL_HIGH);

    /* 3. Initialize SPI Controller at 400 kHz for identification */
    return sd_spi_set_speed(SD_DEFAULT_INIT_SPEED);
}

void sd_spi_hw_deinit(void)
{
    if (s_spi_hd >= 0) {
        nwy_gpio_value_set((uint32_t)s_cs_gpio, PIN_LEVEL_HIGH);
        nwy_spi_deinit(s_spi_hd);
        s_spi_hd = -1;
        s_cur_speed = 0;
    }
    s_card_ready = false;
}

int sd_spi_card_init(void)
{
    uint8_t resp;
    int retry;
    uint32_t ocr = 0;

    s_card_ready = false;
    memset(&s_card_info, 0, sizeof(s_card_info));
    s_card_info.sector_size = 512;

    /* Ensure SPI controller is open and set to 400 kHz identification clock */
    if (s_spi_hd < 0) {
        int ret = sd_spi_hw_init(s_spi_bus, s_cs_gpio);
        if (ret < 0) return ret;
    } else {
        sd_spi_set_speed(SD_DEFAULT_INIT_SPEED);
    }

    /* Configure Pin 79 CS pin idle HIGH */
    nwy_gpio_direction_set((uint32_t)s_cs_gpio, PIN_DIRECTION_OUT);
    nwy_gpio_pull_set((uint32_t)s_cs_gpio, PIN_PULL_PU);
    sd_cs_high();
    sd_delay_ms(10);

    /* Step 1: Send >=74 clock cycles (16 bytes = 128 cycles) with CS HIGH */
    memset(s_dma_tx, 0xFF, 16);
    nwy_spi_transfer(s_spi_hd, SPI_CS_0, s_dma_tx, s_dma_rx, 16);

    /* Step 2: Put card in SPI mode via CMD0 (GO_IDLE_STATE) with CS sync pulsed */
    retry = 0;
    resp = 0xFF;
    do {
        sd_cs_high();
        sd_spi_rx_byte(); /* 8 dummy clock cycles with CS HIGH to sync byte framing */
        sd_cs_low();
        resp = sd_send_cmd(CMD0, 0, 0x95);
        if (resp == 0x01) break;
        sd_delay_ms(2);
    } while (++retry < 50);

    if (resp != 0x01) {
        sd_cs_high();
        s_card_ready = false;
        sd_log("  [SD SPI] CMD0 failed to enter SPI mode (resp=0x%02X)", resp);
        return -1; /* Failed to enter SPI mode (card missing or disconnected) */
    }

    /* Step 3: Send CMD8 (check voltage and SDv2 capability) - CS remains LOW */
    resp = sd_send_cmd(CMD8, 0x000001AA, 0x87);
    if (resp == 0x01) {
        /* SD Version 2.0 or higher: read remaining 4 bytes of R7 */
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) {
            r7[i] = sd_spi_rx_byte();
        }
        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            s_card_info.card_type = SD_CARD_TYPE_SDV2_SC;
        } else {
            sd_cs_high();
            s_card_ready = false;
            sd_log("  [SD SPI] CMD8 voltage pattern mismatch");
            return -2; /* Voltage mismatch */
        }
    } else {
        /* SD Version 1.x or MMC */
        s_card_info.card_type = SD_CARD_TYPE_SDV1;
    }

    /* Step 4: Initialize card using ACMD41 loop - CS remains continuously LOW */
    retry = 0;
    uint32_t acmd41_arg = (s_card_info.card_type == SD_CARD_TYPE_SDV2_SC) ? 0x40000000 : 0x00000000;
    do {
        /* Send CMD55 (APP_CMD) */
        resp = sd_send_cmd(CMD55, 0, 0x65);
        if (resp <= 0x01) {
            /* Send ACMD41 immediately while CS remains continuously LOW */
            resp = sd_send_cmd(ACMD41, acmd41_arg, 0x77);
            if (resp == 0x00) {
                break; /* Card ready! */
            }
        }
        sd_delay_ms(10);
    } while (++retry < 200);

    if (resp != 0x00) {
        sd_cs_high();
        s_card_ready = false;
        sd_log("  [SD SPI] ACMD41 initialization timeout (resp=0x%02X)", resp);
        return -3; /* ACMD41 timeout */
    }

    /* Step 5: If SDv2, query OCR register via CMD58 to check CCS bit - CS remains LOW */
    if (s_card_info.card_type == SD_CARD_TYPE_SDV2_SC) {
        resp = sd_send_cmd(CMD58, 0, 0xFD);
        if (resp == 0x00) {
            uint8_t ocr_bytes[4];
            for (int i = 0; i < 4; i++) {
                ocr_bytes[i] = sd_spi_rx_byte();
            }
            ocr = ((uint32_t)ocr_bytes[0] << 24) |
                  ((uint32_t)ocr_bytes[1] << 16) |
                  ((uint32_t)ocr_bytes[2] << 8)  |
                  (uint32_t)ocr_bytes[3];

            /* Check bit 30 (CCS - Card Capacity Status) */
            if (ocr & 0x40000000) {
                s_card_info.card_type = SD_CARD_TYPE_SDV2_HC;
            }
        }
    }

    /* Step 6: For standard capacity, set block length to 512 - CS remains LOW */
    if (s_card_info.card_type != SD_CARD_TYPE_SDV2_HC) {
        resp = sd_send_cmd(CMD16, 512, 0xFF);
        if (resp != 0x00) {
            sd_cs_high();
            s_card_ready = false;
            return -4;
        }
    }

    /* Step 7: Read CID (CMD10) to obtain card metadata - CS remains LOW */
    resp = sd_send_cmd(CMD10, 0, 0xFF);
    if (resp == 0x00) {
        /* Wait for data token */
        int to = 0;
        while (sd_spi_rx_byte() != TOKEN_START_BLOCK && ++to < 500) {
            sd_delay_ms(1);
        }
        if (to < 500) {
            uint8_t cid[16];
            for (int i = 0; i < 16; i++) {
                cid[i] = sd_spi_rx_byte();
            }
            sd_spi_rx_byte(); /* CRC1 */
            sd_spi_rx_byte(); /* CRC2 */

            s_card_info.manufacturer_id = cid[0];
            s_card_info.oem_id = ((uint16_t)cid[1] << 8) | cid[2];
            memcpy(s_card_info.product_name, &cid[3], 5);
            s_card_info.product_name[5] = '\0';
            s_card_info.revision_major = (cid[8] >> 4) & 0x0F;
            s_card_info.revision_minor = cid[8] & 0x0F;
            s_card_info.serial_number = ((uint32_t)cid[9] << 24) |
                                        ((uint32_t)cid[10] << 16) |
                                        ((uint32_t)cid[11] << 8) |
                                        (uint32_t)cid[12];
            s_card_info.manufacturing_year = 2000 + (((cid[13] & 0x0F) << 4) | ((cid[14] >> 4) & 0x0F));
            s_card_info.manufacturing_month = cid[14] & 0x0F;
        }
    }

    /* Step 8: Read CSD (CMD9) to calculate total capacity and sector count - CS remains LOW */
    resp = sd_send_cmd(CMD9, 0, 0xFF);
    if (resp == 0x00) {
        int to = 0;
        while (sd_spi_rx_byte() != TOKEN_START_BLOCK && ++to < 500) {
            sd_delay_ms(1);
        }
        if (to < 500) {
            uint8_t csd[16];
            for (int i = 0; i < 16; i++) {
                csd[i] = sd_spi_rx_byte();
            }
            sd_spi_rx_byte(); /* CRC1 */
            sd_spi_rx_byte(); /* CRC2 */

            uint8_t csd_ver = (csd[0] >> 6) & 0x03;
            if (csd_ver == 1) {
                /* CSD Version 2.0 (SDHC / SDXC) */
                uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                                  ((uint32_t)csd[8] << 8) |
                                  (uint32_t)csd[9];
                s_card_info.capacity_bytes = (uint64_t)(c_size + 1) * 512 * 1024;
                s_card_info.sector_count = (uint32_t)(s_card_info.capacity_bytes / 512);
            } else {
                /* CSD Version 1.0 (Standard Capacity) */
                uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                                  ((uint32_t)csd[7] << 2) |
                                  ((uint32_t)(csd[8] & 0xC0) >> 6);
                uint8_t c_size_mult = ((csd[9] & 0x03) << 1) | ((csd[10] & 0x80) >> 7);
                uint8_t read_bl_len = csd[5] & 0x0F;
                uint32_t blocknr = (c_size + 1) * (1 << (c_size_mult + 2));
                s_card_info.capacity_bytes = (uint64_t)blocknr * (1 << read_bl_len);
                s_card_info.sector_count = (uint32_t)(s_card_info.capacity_bytes / 512);
            }
        }
    }

    /* Release CS line */
    sd_cs_high();

    /* Step 9: Switch to high-speed SPI clock (12 MHz) */
    int ret_spd = sd_spi_set_speed(SD_DEFAULT_WORK_SPEED);
    if (ret_spd < 0) {
        return ret_spd;
    }

    s_card_ready = true;
    return 0;
}

int sd_spi_read_sector(uint32_t sector, uint8_t *buffer, uint32_t count)
{
    if (!s_card_ready || buffer == NULL || count == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_addr = (s_card_info.card_type == SD_CARD_TYPE_SDV2_HC) ? (sector + i) : ((sector + i) * 512);
        sd_cs_low();

        uint8_t resp = sd_send_cmd(CMD17, cur_addr, 0xFF);
        if (resp != 0x00) {
            sd_cs_high();
            s_card_ready = false;
            return -2;
        }

        /* Wait for data start token 0xFE */
        int to = 0;
        uint8_t token;
        do {
            token = sd_spi_rx_byte();
            if (token == TOKEN_START_BLOCK) break;
            sd_delay_ms(1);
        } while (++to < 200);

        if (token != TOKEN_START_BLOCK) {
            sd_cs_high();
            s_card_ready = false;
            return -3; /* Token timeout */
        }

        /* Read 512 bytes using DMA aligned buffer */
        memset(s_dma_tx, 0xFF, 512);
        nwy_spi_transfer(s_spi_hd, SPI_CS_0, s_dma_tx, s_dma_rx, 512);
        memcpy(buffer + (i * 512), s_dma_rx, 512);

        /* Read and discard 2-byte CRC */
        sd_spi_rx_byte();
        sd_spi_rx_byte();

        sd_cs_high();
    }

    return 0;
}

int sd_spi_write_sector(uint32_t sector, const uint8_t *buffer, uint32_t count)
{
    if (!s_card_ready || buffer == NULL || count == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_addr = (s_card_info.card_type == SD_CARD_TYPE_SDV2_HC) ? (sector + i) : ((sector + i) * 512);
        sd_cs_low();

        uint8_t resp = sd_send_cmd(CMD24, cur_addr, 0xFF);
        if (resp != 0x00) {
            sd_cs_high();
            s_card_ready = false;
            return -2;
        }

        /* Send 1 dummy byte then Data Start Token (0xFE) */
        sd_spi_transfer_byte(0xFF);
        sd_spi_transfer_byte(TOKEN_START_BLOCK);

        /* Transfer 512 bytes via DMA aligned buffer */
        memcpy(s_dma_tx, buffer + (i * 512), 512);
        nwy_spi_transfer(s_spi_hd, SPI_CS_0, s_dma_tx, s_dma_rx, 512);

        /* Send dummy 2-byte CRC */
        sd_spi_transfer_byte(0xFF);
        sd_spi_transfer_byte(0xFF);

        /* Read data response byte */
        uint8_t data_resp = sd_spi_rx_byte();
        if ((data_resp & 0x1F) != TOKEN_DATA_ACCEPTED) {
            sd_cs_high();
            s_card_ready = false;
            return -3; /* Data rejected by card */
        }

        /* Wait until card finishes internal flash programming (busy = 0x00) */
        if (sd_spi_wait_ready(SD_BUSY_TIMEOUT_MS) != 0) {
            sd_cs_high();
            s_card_ready = false;
            return -4; /* Programming timeout */
        }

        sd_cs_high();
    }

    return 0;
}

int sd_spi_sync(void)
{
    if (!s_card_ready) return -1;
    sd_cs_low();
    int ret = sd_spi_wait_ready(SD_BUSY_TIMEOUT_MS);
    sd_cs_high();
    if (ret != 0) {
        s_card_ready = false;
    }
    return ret;
}

const sd_card_info_t *sd_spi_get_info(void)
{
    if (!s_card_ready) return NULL;
    return &s_card_info;
}

bool sd_spi_is_ready(void)
{
    return s_card_ready;
}

void sd_spi_set_unready(void)
{
    s_card_ready = false;
}

bool sd_spi_ping(void)
{
    if (!s_card_ready || s_spi_hd < 0) {
        return false;
    }
    sd_cs_low();
    /* Send CMD13 (SEND_STATUS, 2 bytes arg 0) */
    uint8_t resp = sd_send_cmd(13, 0, 0xFF);
    if (resp == 0x00) {
        sd_spi_rx_byte(); /* R2 second byte */
        sd_cs_high();
        return true;
    }
    sd_cs_high();
    s_card_ready = false;
    return false;
}
