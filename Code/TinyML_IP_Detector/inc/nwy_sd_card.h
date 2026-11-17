#ifndef __NWY_SD_CARD_H__
#define __NWY_SD_CARD_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sd_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         spi_bus_id;       /* 1 for "SPI1" */
    int         cs_gpio;          /* 34 for Pin 79 */
    uint32_t    init_speed;       /* 400000 Hz */
    uint32_t    work_speed;       /* 12000000 Hz */
    const char *mount_point;      /* "0:" */
} nwy_sdcard_config_t;

int nwy_sdcard_init(const nwy_sdcard_config_t *config);
void nwy_sdcard_deinit(void);
bool nwy_sdcard_is_mounted(void);
int nwy_sdcard_reinit(void);
bool nwy_sdcard_is_present(void);
bool nwy_sdcard_lock(uint32_t timeout_ms);
void nwy_sdcard_unlock(void);
bool nwy_sdcard_exists(const char *path);
int nwy_sdcard_create_dir(const char *path);
int nwy_sdcard_remove_dir(const char *path);
int nwy_sdcard_remove_dir_recursive(const char *path);
int nwy_sdcard_delete_file(const char *path);
int nwy_sdcard_read_file(const char *path, char *buffer, size_t buffer_size, size_t *out_read_len);
int nwy_sdcard_write_file(const char *path, const char *data, size_t len);
int nwy_sdcard_append_file(const char *path, const char *data, size_t len);
uint64_t nwy_sdcard_get_total_space(void);
uint64_t nwy_sdcard_get_free_space(void);
int nwy_sdcard_format(void);

#ifdef __cplusplus
}
#endif

#endif /* __NWY_SD_CARD_H__ */
