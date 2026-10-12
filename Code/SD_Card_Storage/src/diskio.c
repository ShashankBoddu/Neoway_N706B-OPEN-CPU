/*-----------------------------------------------------------------------*/
/* Low level disk I/O module glue for FatFs R0.12c on Neoway OpenCPU     */
/*-----------------------------------------------------------------------*/

#include "diskio.h"
#include "sd_spi.h"

#define DEV_SD  0

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != DEV_SD) return STA_NOINIT;

    if (!sd_spi_is_ready()) {
        return STA_NOINIT;
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != DEV_SD) return STA_NOINIT;

    if (sd_spi_is_ready()) {
        return 0;
    }

    int ret = sd_spi_card_init();
    if (ret != 0) {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != DEV_SD || count == 0 || buff == 0) {
        return RES_PARERR;
    }

    if (!sd_spi_is_ready()) {
        return RES_NOTRDY;
    }

    int ret = sd_spi_read_sector(sector, buff, count);
    if (ret != 0) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != DEV_SD || count == 0 || buff == 0) {
        return RES_PARERR;
    }

    if (!sd_spi_is_ready()) {
        return RES_NOTRDY;
    }

    int ret = sd_spi_write_sector(sector, buff, count);
    if (ret != 0) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != DEV_SD) return RES_PARERR;

    if (!sd_spi_is_ready()) {
        return RES_NOTRDY;
    }

    const sd_card_info_t *info = sd_spi_get_info();
    if (!info) return RES_ERROR;

    switch (cmd) {
    case CTRL_SYNC:
        if (sd_spi_sync() == 0) {
            return RES_OK;
        }
        return RES_ERROR;

    case GET_SECTOR_COUNT:
        if (buff == 0) return RES_PARERR;
        *(DWORD *)buff = info->sector_count;
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == 0) return RES_PARERR;
        *(WORD *)buff = 512;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (buff == 0) return RES_PARERR;
        *(DWORD *)buff = 1; /* 1 sector per erase block default */
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
