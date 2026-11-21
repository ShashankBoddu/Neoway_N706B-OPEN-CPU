/**
 * @file remote_sd_manager.h
 * @brief Neoway N706B Remote SD Card File Manager & HTTP Data Uploader
 * 
 * Provides remote file listing and HTTP uploading of Bosch BME AI-Studio 
 * datasets (.bmerawdata, .bmelabelinfo, .csv) over cellular LTE connection.
 */

#ifndef __REMOTE_SD_MANAGER_H__
#define __REMOTE_SD_MANAGER_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_HTTP_SERVER_URL "http://api.fervidlabs.in/upload"

typedef enum {
    SD_UPLOAD_IDLE = 0,
    SD_UPLOAD_BUSY,
    SD_UPLOAD_SUCCESS,
    SD_UPLOAD_ERROR
} sd_upload_state_e;

typedef enum {
    UPLOAD_TARGET_SINGLE = 1,
    UPLOAD_TARGET_LATEST = 2,
    UPLOAD_TARGET_ALL    = 3
} sd_upload_target_type_e;

typedef struct {
    sd_upload_target_type_e target_type;
    char                    filename[96];
    char                    server_url[192];
    char                    notify_phone[32];
} sd_upload_request_t;

/**
 * @brief Initialize remote SD manager and spawn background upload worker thread.
 * @return 0 on success, negative on error.
 */
int remote_sd_manager_init(void);

/**
 * @brief Scan MicroSD card root directory and summarize dataset files.
 * @param summary_out Output buffer for human-readable SMS/console summary.
 * @param max_len Maximum length of summary_out buffer.
 * @return Total number of matching files found.
 */
int remote_sd_list_files(char *summary_out, size_t max_len);

/**
 * @brief Configure target HTTP server upload endpoint.
 * @param url Full HTTP URL (e.g. "http://192.168.1.100:8080/upload" or "http://api.myserver.com/upload")
 */
void remote_sd_set_server_url(const char *url);

/**
 * @brief Get currently configured target HTTP server upload endpoint.
 * @return Pointer to URL string.
 */
const char *remote_sd_get_server_url(void);

/**
 * @brief Enqueue a background HTTP upload request.
 * @param req Pointer to upload request parameters.
 * @return true if queued, false if uploader busy.
 */
bool remote_sd_queue_upload(const sd_upload_request_t *req);

/**
 * @brief Direct synchronous upload of a single file from SD card to HTTP server.
 * @param filename File name on SD card (e.g. "20261117_1336_IP_0.bmerawdata" or full path "0:/...")
 * @param server_url HTTP server destination URL (NULL to use configured default)
 * @param notify_phone Optional phone number to notify via SMS upon completion (NULL to disable)
 * @return 0 on success, negative on error.
 */
int remote_sd_upload_file_sync(const char *filename, const char *server_url, const char *notify_phone);

/**
 * @brief Upload the most recently recorded session files (.bmerawdata and .bmelabelinfo).
 * @param server_url HTTP server destination URL (NULL to use configured default)
 * @param notify_phone Optional phone number to notify via SMS upon completion (NULL to disable)
 * @return 0 on success, negative on error.
 */
int remote_sd_upload_latest_sync(const char *server_url, const char *notify_phone);

/**
 * @brief Upload all dataset files (.bmerawdata and .bmelabelinfo) found on the SD card.
 * @param server_url HTTP server destination URL (NULL to use configured default)
 * @param notify_phone Optional phone number to notify via SMS upon completion (NULL to disable)
 * @return Number of files uploaded, or negative on error.
 */
int remote_sd_upload_all_sync(const char *server_url, const char *notify_phone);

/**
 * @brief Check current upload status and progress.
 * @param state_out Output pointer for current state enum
 * @param bytes_sent Output pointer for transmitted byte count
 * @param total_bytes Output pointer for total file size in bytes
 */
void remote_sd_get_upload_status(sd_upload_state_e *state_out, uint32_t *bytes_sent, uint32_t *total_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __REMOTE_SD_MANAGER_H__ */
