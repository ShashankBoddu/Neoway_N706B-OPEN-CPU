#include "nwy_sd_card.h"
#include "ff.h"
#include "nwy_osi_api.h"
#include <string.h>
#include <stdio.h>

static FATFS               s_fatfs;
static bool                s_is_mounted = false;
static nwy_sdcard_config_t s_config;

/* Rate-limiting interval for auto-recovery when card is absent (1 second) */
#define SD_REINIT_COOLOFF_MS    1000
static int64               s_last_reinit_attempt_ms = 0;

/* Recursive Mutex Context */
static nwy_osi_mutex_t     s_raw_mutex = NULL;
static nwy_osi_thread_t    s_mutex_owner = NULL;
static uint32_t            s_recursion_count = 0;

static void format_path(const char *in_path, char *out_buf, size_t max_len)
{
    if (in_path == NULL || out_buf == NULL || max_len == 0) return;

    /* If path already begins with "0:", copy directly */
    if (strncmp(in_path, "0:", 2) == 0) {
        strncpy(out_buf, in_path, max_len - 1);
        out_buf[max_len - 1] = '\0';
        return;
    }

    /* Otherwise prefix with "0:" */
    if (in_path[0] == '/') {
        snprintf(out_buf, max_len, "0:%s", in_path);
    } else {
        snprintf(out_buf, max_len, "0:/%s", in_path);
    }
}

bool nwy_sdcard_lock(uint32_t timeout_ms)
{
    if (s_raw_mutex == NULL) return false;

    nwy_osi_thread_t cur_thread = nwy_thread_get_current();

    if (s_mutex_owner == cur_thread) {
        s_recursion_count++;
        return true;
    }

    nwy_error_e err = nwy_sdk_mutex_lock(s_raw_mutex, (int)timeout_ms);
    if (err == NWY_SUCCESS) {
        s_mutex_owner = cur_thread;
        s_recursion_count = 1;
        return true;
    }
    return false;
}

void nwy_sdcard_unlock(void)
{
    if (s_raw_mutex == NULL) return;

    nwy_osi_thread_t cur_thread = nwy_thread_get_current();

    if (s_mutex_owner == cur_thread) {
        if (s_recursion_count > 1) {
            s_recursion_count--;
        } else {
            s_recursion_count = 0;
            s_mutex_owner = NULL;
            nwy_sdk_mutex_unlock(s_raw_mutex);
        }
    }
}

static bool sd_should_attempt_reinit(void)
{
    int64 now = nwy_uptime_get();
    if (s_last_reinit_attempt_ms == 0 ||
        (now - s_last_reinit_attempt_ms) >= SD_REINIT_COOLOFF_MS ||
        (now < s_last_reinit_attempt_ms)) {
        return true;
    }
    return false;
}

static int sd_safe_reinit_locked(void)
{
    s_last_reinit_attempt_ms = nwy_uptime_get();
    s_is_mounted = false;
    sd_spi_set_unready();

    /* 1. Safely unmount existing FatFs volume */
    f_mount(NULL, "0:", 0);
    memset(&s_fatfs, 0, sizeof(s_fatfs));

    /* 2. Execute SPI card reset & re-identification sequence */
    int ret = sd_spi_card_init();
    if (ret != 0) {
        return -1; /* Card not responding in SPI mode (absent or disconnected) */
    }

    /* 3. Mount FatFs volume cleanly */
    FRESULT fr = f_mount(&s_fatfs, "0:", 1);
    if (fr != FR_OK) {
        sd_spi_set_unready();
        return -2;
    }

    s_is_mounted = true;
    return 0;
}

int nwy_sdcard_init(const nwy_sdcard_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    } else {
        s_config.spi_bus_id   = SD_DEFAULT_SPI_BUS;
        s_config.cs_gpio      = SD_DEFAULT_CS_GPIO;
        s_config.init_speed   = SD_DEFAULT_INIT_SPEED;
        s_config.work_speed   = SD_DEFAULT_WORK_SPEED;
        s_config.mount_point  = "0:";
    }

    /* 1. Create Recursive Mutex */
    if (s_raw_mutex == NULL) {
        nwy_sdk_mutex_create(&s_raw_mutex);
        s_mutex_owner = NULL;
        s_recursion_count = 0;
    }

    if (!nwy_sdcard_lock(3000)) return -1;

    if (s_is_mounted) {
        nwy_sdcard_unlock();
        return 0;
    }

    /* 2. Initialize low-level SPI controller */
    int ret = sd_spi_hw_init(s_config.spi_bus_id, s_config.cs_gpio);
    if (ret != 0) {
        nwy_sdcard_unlock();
        return -2;
    }

    ret = sd_safe_reinit_locked();
    if (ret != 0) {
        nwy_sdcard_unlock();
        return -3;
    }

    /* 3. Ensure reference directory structure and FIFO pointers exist */
    if (!nwy_sdcard_exists("/unpublish"))       nwy_sdcard_create_dir("/unpublish");
    if (!nwy_sdcard_exists("/unpublish/data"))  nwy_sdcard_create_dir("/unpublish/data");
    if (!nwy_sdcard_exists("/TC_config"))       nwy_sdcard_create_dir("/TC_config");
    if (!nwy_sdcard_exists("/TU_config"))       nwy_sdcard_create_dir("/TU_config");
    if (!nwy_sdcard_exists("/tu_cache"))        nwy_sdcard_create_dir("/tu_cache");
    if (!nwy_sdcard_exists("/bin"))             nwy_sdcard_create_dir("/bin");
    if (!nwy_sdcard_exists("/bin/tc"))          nwy_sdcard_create_dir("/bin/tc");
    if (!nwy_sdcard_exists("/bin/tu"))          nwy_sdcard_create_dir("/bin/tu");

    if (!nwy_sdcard_exists("/unpublish/head.txt")) nwy_sdcard_write_file("/unpublish/head.txt", "0", 1);
    if (!nwy_sdcard_exists("/unpublish/tail.txt")) nwy_sdcard_write_file("/unpublish/tail.txt", "0", 1);
    if (nwy_sdcard_exists("/unpublish/NumOf.txt"))  nwy_sdcard_delete_file("/unpublish/NumOf.txt");

    nwy_sdcard_unlock();
    return 0;
}

void nwy_sdcard_deinit(void)
{
    if (!nwy_sdcard_lock(2000)) return;

    if (s_is_mounted) {
        f_mount(NULL, "0:", 0);
        s_is_mounted = false;
        sd_spi_hw_deinit();
    }

    nwy_sdcard_unlock();
}

bool nwy_sdcard_is_mounted(void)
{
    return s_is_mounted;
}

int nwy_sdcard_reinit(void)
{
    if (!nwy_sdcard_lock(3000)) return -1;
    int ret = sd_safe_reinit_locked();
    nwy_sdcard_unlock();
    return ret;
}

bool nwy_sdcard_is_present(void)
{
    if (!nwy_sdcard_lock(1000)) return false;
    bool present = sd_spi_ping();
    nwy_sdcard_unlock();
    return present;
}

bool nwy_sdcard_exists(const char *path)
{
    if (path == NULL) return false;
    if (!nwy_sdcard_lock(2000)) return false;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return false;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FILINFO fno;
    FRESULT fr = f_stat(fpath, &fno);
    if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_stat(fpath, &fno);
        }
    }

    nwy_sdcard_unlock();
    return (fr == FR_OK);
}

int nwy_sdcard_create_dir(const char *path)
{
    if (path == NULL) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FRESULT fr = f_mkdir(fpath);
    if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_mkdir(fpath);
        }
    }

    nwy_sdcard_unlock();
    return (fr == FR_OK || fr == FR_EXIST) ? 0 : -4;
}

int nwy_sdcard_remove_dir(const char *path)
{
    if (path == NULL) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FRESULT fr = f_unlink(fpath);
    if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_unlink(fpath);
        }
    }

    nwy_sdcard_unlock();
    return (fr == FR_OK) ? 0 : -4;
}

int nwy_sdcard_remove_dir_recursive(const char *path)
{
    if (path == NULL) return -1;
    if (!nwy_sdcard_lock(3000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, fpath);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
            s_is_mounted = false;
        }
        nwy_sdcard_unlock();
        return -4;
    }

    while (1) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.') continue;

        char sub_path[128];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", fpath, fno.fname);

        if (fno.fattrib & AM_DIR) {
            nwy_sdcard_remove_dir_recursive(sub_path);
        } else {
            f_unlink(sub_path);
        }
    }
    f_closedir(&dir);

    fr = f_unlink(fpath);
    nwy_sdcard_unlock();
    return (fr == FR_OK) ? 0 : -5;
}

int nwy_sdcard_delete_file(const char *path)
{
    if (path == NULL) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FRESULT fr = f_unlink(fpath);
    if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_unlink(fpath);
        }
    }

    nwy_sdcard_unlock();
    return (fr == FR_OK || fr == FR_NO_FILE) ? 0 : -4;
}

int nwy_sdcard_read_file(const char *path, char *buffer, size_t buffer_size, size_t *out_read_len)
{
    if (path == NULL || buffer == NULL || buffer_size == 0) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FIL fil;
    FRESULT fr = f_open(&fil, fpath, FA_READ);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
            s_is_mounted = false;
            if (sd_safe_reinit_locked() == 0) {
                fr = f_open(&fil, fpath, FA_READ);
            }
        }
    }

    if (fr != FR_OK) {
        nwy_sdcard_unlock();
        return -4;
    }

    UINT br = 0;
    fr = f_read(&fil, buffer, (UINT)(buffer_size - 1), &br);
    buffer[br] = '\0';
    f_close(&fil);

    if (fr != FR_OK) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            FRESULT rfr = f_open(&fil, fpath, FA_READ);
            if (rfr == FR_OK) {
                UINT rbr = 0;
                rfr = f_read(&fil, buffer, (UINT)(buffer_size - 1), &rbr);
                buffer[rbr] = '\0';
                f_close(&fil);
                if (rfr == FR_OK) {
                    if (out_read_len != NULL) *out_read_len = (size_t)rbr;
                    nwy_sdcard_unlock();
                    return 0;
                }
            }
        }
        nwy_sdcard_unlock();
        return -5;
    }

    if (out_read_len != NULL) {
        *out_read_len = (size_t)br;
    }

    nwy_sdcard_unlock();
    return 0;
}

int nwy_sdcard_write_file(const char *path, const char *data, size_t len)
{
    if (path == NULL || data == NULL) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FIL fil;
    FRESULT fr = f_open(&fil, fpath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
            s_is_mounted = false;
            if (sd_safe_reinit_locked() == 0) {
                fr = f_open(&fil, fpath, FA_CREATE_ALWAYS | FA_WRITE);
            }
        }
    }

    if (fr != FR_OK) {
        nwy_sdcard_unlock();
        return -4;
    }

    UINT bw = 0;
    fr = f_write(&fil, data, (UINT)len, &bw);
    f_close(&fil);

    if (fr != FR_OK || bw != len) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            FRESULT rfr = f_open(&fil, fpath, FA_CREATE_ALWAYS | FA_WRITE);
            if (rfr == FR_OK) {
                UINT rbw = 0;
                rfr = f_write(&fil, data, (UINT)len, &rbw);
                f_close(&fil);
                if (rfr == FR_OK && rbw == len) {
                    nwy_sdcard_unlock();
                    return 0;
                }
            }
        }
        nwy_sdcard_unlock();
        return -5;
    }

    nwy_sdcard_unlock();
    return 0;
}

int nwy_sdcard_append_file(const char *path, const char *data, size_t len)
{
    if (path == NULL || data == NULL) return -1;
    if (!nwy_sdcard_lock(2000)) return -2;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return -3;
        }
    }

    char fpath[128];
    format_path(path, fpath, sizeof(fpath));

    FIL fil;
    FRESULT fr = f_open(&fil, fpath, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR || fr == FR_NOT_READY || fr == FR_INT_ERR) {
            s_is_mounted = false;
            if (sd_safe_reinit_locked() == 0) {
                fr = f_open(&fil, fpath, FA_OPEN_ALWAYS | FA_WRITE);
            }
        }
    }

    if (fr != FR_OK) {
        nwy_sdcard_unlock();
        return -4;
    }

    /* Seek to end of file */
    f_lseek(&fil, f_size(&fil));

    UINT bw = 0;
    fr = f_write(&fil, data, (UINT)len, &bw);
    f_close(&fil);

    if (fr != FR_OK || bw != len) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            FRESULT rfr = f_open(&fil, fpath, FA_OPEN_ALWAYS | FA_WRITE);
            if (rfr == FR_OK) {
                f_lseek(&fil, f_size(&fil));
                UINT rbw = 0;
                rfr = f_write(&fil, data, (UINT)len, &rbw);
                f_close(&fil);
                if (rfr == FR_OK && rbw == len) {
                    nwy_sdcard_unlock();
                    return 0;
                }
            }
        }
        nwy_sdcard_unlock();
        return -5;
    }

    nwy_sdcard_unlock();
    return 0;
}

uint64_t nwy_sdcard_get_total_space(void)
{
    if (!nwy_sdcard_lock(2000)) return 0;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return 0;
        }
    }

    FATFS *fs = NULL;
    DWORD fre_clust = 0;
    FRESULT fr = f_getfree("0:", &fre_clust, &fs);
    if (fr != FR_OK || fs == NULL) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_getfree("0:", &fre_clust, &fs);
        }
    }

    if (fr != FR_OK || fs == NULL) {
        nwy_sdcard_unlock();
        return 0;
    }

    DWORD tot_sect = (fs->n_fatent - 2) * fs->csize;
    uint64_t total_bytes = (uint64_t)tot_sect * 512;

    nwy_sdcard_unlock();
    return total_bytes;
}

uint64_t nwy_sdcard_get_free_space(void)
{
    if (!nwy_sdcard_lock(2000)) return 0;

    if (!s_is_mounted) {
        if (sd_should_attempt_reinit()) {
            sd_safe_reinit_locked();
        }
        if (!s_is_mounted) {
            nwy_sdcard_unlock();
            return 0;
        }
    }

    FATFS *fs = NULL;
    DWORD fre_clust = 0;
    FRESULT fr = f_getfree("0:", &fre_clust, &fs);
    if (fr != FR_OK || fs == NULL) {
        s_is_mounted = false;
        if (sd_safe_reinit_locked() == 0) {
            fr = f_getfree("0:", &fre_clust, &fs);
        }
    }

    if (fr != FR_OK || fs == NULL) {
        nwy_sdcard_unlock();
        return 0;
    }

    DWORD fre_sect = fre_clust * fs->csize;
    uint64_t free_bytes = (uint64_t)fre_sect * 512;

    nwy_sdcard_unlock();
    return free_bytes;
}

int nwy_sdcard_format(void)
{
    if (!nwy_sdcard_lock(5000)) return -1;

    BYTE work[512];
    FRESULT fr = f_mkfs("0:", FM_FAT32, 0, work, sizeof(work));
    if (fr == FR_OK) {
        f_mount(&s_fatfs, "0:", 1);
        s_is_mounted = true;
    }

    nwy_sdcard_unlock();
    return (fr == FR_OK) ? 0 : -2;
}
