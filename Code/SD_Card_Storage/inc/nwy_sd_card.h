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

/**
 * @brief Initialize low-level bus, mount the SD card via FatFs, and create default directories.
 * @param config Pointer to configuration struct (pass NULL for hardware defaults).
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_init(const nwy_sdcard_config_t *config);

/**
 * @brief Unmount the card and release filesystem resources.
 */
void nwy_sdcard_deinit(void);

/**
 * @brief Check if the SD card is currently mounted and operational.
 */
bool nwy_sdcard_is_mounted(void);

/**
 * @brief Safely re-initialize the SD card over SPI and re-mount FatFs.
 *        Can be called on demand or upon recovering from a hot-swap disconnect.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_reinit(void);

/**
 * @brief Check if the SD card is physically present and responsive over SPI.
 * @return true if responsive, false if missing or error.
 */
bool nwy_sdcard_is_present(void);

/**
 * @brief Thread-safe recursive mutex lock for SD storage operations.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if lock acquired, false on timeout or error.
 */
bool nwy_sdcard_lock(uint32_t timeout_ms);

/**
 * @brief Release the recursive mutex.
 */
void nwy_sdcard_unlock(void);

/**
 * @brief Check if a file or directory exists on the card.
 * @param path Path (e.g. "/unpublish" or "/data/log.txt")
 * @return true if exists, false otherwise.
 */
bool nwy_sdcard_exists(const char *path);

/**
 * @brief Create a directory.
 * @param path Directory path to create.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_create_dir(const char *path);

/**
 * @brief Remove an empty directory.
 * @param path Directory path to remove.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_remove_dir(const char *path);

/**
 * @brief Recursively remove a directory and all contained files/subdirectories.
 * @param path Directory path to recursively remove.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_remove_dir_recursive(const char *path);

/**
 * @brief Delete a file from the SD card.
 * @param path File path to delete.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_delete_file(const char *path);

/**
 * @brief Read file content into a buffer.
 * @param path File path.
 * @param buffer Output buffer.
 * @param buffer_size Maximum buffer capacity.
 * @param out_read_len Optional pointer to receive actual bytes read.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_read_file(const char *path, char *buffer, size_t buffer_size, size_t *out_read_len);

/**
 * @brief Write data to a file (creates or overwrites existing content).
 * @param path File path.
 * @param data Data string or byte buffer to write.
 * @param len Length of data in bytes.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_write_file(const char *path, const char *data, size_t len);

/**
 * @brief Append data to the end of an existing or new file.
 * @param path File path.
 * @param data Data string or byte buffer to append.
 * @param len Length of data in bytes.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_append_file(const char *path, const char *data, size_t len);

/**
 * @brief Get total capacity of the mounted volume in bytes.
 * @return Total bytes, or 0 on error.
 */
uint64_t nwy_sdcard_get_total_space(void);

/**
 * @brief Get remaining free space on the mounted volume in bytes.
 * @return Free bytes, or 0 on error.
 */
uint64_t nwy_sdcard_get_free_space(void);

/**
 * @brief Format the SD card volume with FAT filesystem.
 * @return 0 on success, negative error code on failure.
 */
int nwy_sdcard_format(void);

#ifdef __cplusplus
}
#endif

#endif /* __NWY_SD_CARD_H__ */
