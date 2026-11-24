/**
 * @file remote_sd_manager.c
 * @brief Neoway N706B Remote SD Card File Manager & HTTP Data Uploader
 */

#include "remote_sd_manager.h"
#include "data_collector.h"
#include "nwy_sd_card.h"
#include "ff.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/net/nwy_hal_net.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/uart/nwy_hal_uart.h"
#include "nwy_osi_api.h"
#include "nwy_socket_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#define TAG "REMOTE_SD"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

#define UPLOAD_CHUNK_SIZE 1024

static char s_configured_server_url[192] = DEFAULT_HTTP_SERVER_URL;

static bool has_dataset_extension(const char *name)
{
    if (!name) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;

    char ext_lower[16];
    size_t i = 0;
    for (; ext[i] && i < sizeof(ext_lower) - 1; i++) {
        ext_lower[i] = (char)tolower((unsigned char)ext[i]);
    }
    ext_lower[i] = '\0';

    return (strcmp(ext_lower, ".bmerawdata") == 0 ||
            strcmp(ext_lower, ".bmelabelinfo") == 0 ||
            strcmp(ext_lower, ".csv") == 0);
}

static bool is_bmerawdata_ext(const char *name)
{
    if (!name) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;

    char ext_lower[16];
    size_t i = 0;
    for (; ext[i] && i < sizeof(ext_lower) - 1; i++) {
        ext_lower[i] = (char)tolower((unsigned char)ext[i]);
    }
    ext_lower[i] = '\0';

    return (strcmp(ext_lower, ".bmerawdata") == 0);
}

/* State & Progress Tracking */
static sd_upload_state_e  s_upload_state = SD_UPLOAD_IDLE;
static uint32_t           s_bytes_sent = 0;
static uint32_t           s_total_bytes = 0;

/* Worker Thread Synchronization */
static nwy_osi_semaphore_t s_upload_sem = NULL;
static nwy_osi_thread_t    s_upload_thread = NULL;
static sd_upload_request_t s_pending_req;
static volatile bool       s_has_pending_req = false;

/* Helper: parse HTTP URL into host, port, uri */
static bool parse_http_url(const char *url, char *host, size_t host_len, uint16_t *port, char *uri, size_t uri_len)
{
    if (!url || !host || !port || !uri) return false;

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "tcp://", 6) == 0) {
        p += 6;
    }

    *port = 80;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        /* Host with explicit port */
        size_t hlen = colon - p;
        if (hlen >= host_len) hlen = host_len - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';

        *port = (uint16_t)atoi(colon + 1);
        if (slash) {
            strncpy(uri, slash, uri_len - 1);
            uri[uri_len - 1] = '\0';
        } else {
            strncpy(uri, "/upload", uri_len - 1);
            uri[uri_len - 1] = '\0';
        }
    } else if (slash) {
        /* Host without explicit port */
        size_t hlen = slash - p;
        if (hlen >= host_len) hlen = host_len - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';

        strncpy(uri, slash, uri_len - 1);
        uri[uri_len - 1] = '\0';
    } else {
        /* Bare host */
        strncpy(host, p, host_len - 1);
        host[host_len - 1] = '\0';
        strncpy(uri, "/upload", uri_len - 1);
        uri[uri_len - 1] = '\0';
    }

    return true;
}

/* Helper: ensure cellular data call is active */
static bool ensure_data_connection(void)
{
    char ip_buf[32] = {0};
    if (nwy_hal_net_get_ip(1, 1, ip_buf, sizeof(ip_buf)) && strlen(ip_buf) > 4) {
        LOGI("Data connection active. Local IP: %s", ip_buf);
        return true;
    }

    LOGI("Data call inactive. Dialing LTE PDP context profile 1...");
    nwy_hal_net_start_data_call(1, 1, NULL);

    int wait_cycles = 0;
    while (wait_cycles < 15) {
        nwy_hal_os_thread_sleep(1000);
        wait_cycles++;
        if (nwy_hal_net_get_ip(1, 1, ip_buf, sizeof(ip_buf)) && strlen(ip_buf) > 4) {
            LOGI("Data call established after %d seconds! IP: %s", wait_cycles, ip_buf);
            return true;
        }
    }

    LOGE("Data call dial timeout after 15s.");
    return false;
}

void remote_sd_set_server_url(const char *url)
{
    if (url && strlen(url) > 0) {
        strncpy(s_configured_server_url, url, sizeof(s_configured_server_url) - 1);
        s_configured_server_url[sizeof(s_configured_server_url) - 1] = '\0';
        LOGI("Target upload server updated: %s", s_configured_server_url);
    }
}

const char *remote_sd_get_server_url(void)
{
    return s_configured_server_url;
}

int remote_sd_list_files(char *summary_out, size_t max_len)
{
    DIR dir;
    FILINFO fno;
    int count = 0;
    uint32_t total_size = 0;
    char latest_name[64] = {0};

    LOGI("Listing MicroSD card root directory (0:/)...");

    if (!nwy_sdcard_lock(5000)) {
        LOGE("Failed to acquire SD card mutex for listing!");
        if (summary_out && max_len > 0) {
            snprintf(summary_out, max_len, "ERROR: MicroSD card is busy.");
        }
        return -1;
    }

    FRESULT res = f_opendir(&dir, "0:/");
    if (res != FR_OK) {
        nwy_sdcard_unlock();
        LOGE("Failed to open SD card root directory! FatFs err: %d", res);
        if (summary_out && max_len > 0) {
            snprintf(summary_out, max_len, "ERROR: MicroSD directory read failed (err: %d).", res);
        }
        return -1;
    }

    char first_names[3][64];
    uint32_t first_sizes[3] = {0};
    int preview_count = 0;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (!(fno.fattrib & AM_DIR)) {
            if (has_dataset_extension(fno.fname)) {
                count++;
                total_size += (uint32_t)fno.fsize;
                strncpy(latest_name, fno.fname, sizeof(latest_name) - 1);

                LOGI("  [%d] %-36s (%lu bytes)", count, fno.fname, (unsigned long)fno.fsize);

                if (preview_count < 3) {
                    strncpy(first_names[preview_count], fno.fname, sizeof(first_names[preview_count]) - 1);
                    first_sizes[preview_count] = (uint32_t)fno.fsize;
                    preview_count++;
                }
            }
        }
    }
    f_closedir(&dir);
    nwy_sdcard_unlock();

    LOGI("MicroSD scan complete: %d dataset files found, total %lu bytes", count, (unsigned long)total_size);

    if (summary_out && max_len > 0) {
        if (count == 0) {
            snprintf(summary_out, max_len, "SD Files: 0 datasets found on MicroSD card.");
        } else if (count <= 3) {
            int off = snprintf(summary_out, max_len, "SD Files (%d):\n", count);
            for (int i = 0; i < preview_count && off < (int)max_len - 30; i++) {
                uint32_t kb = (first_sizes[i] + 1023) / 1024;
                off += snprintf(summary_out + off, max_len - off, "%d.%s(%luK)\n",
                                i + 1, first_names[i], (unsigned long)kb);
            }
        } else {
            uint32_t total_kb = (total_size + 1023) / 1024;
            snprintf(summary_out, max_len,
                     "SD Files: %d found (%luKB). Latest: %s. Send 'UPLOAD LATEST' or 'UPLOAD ALL'.",
                     count, (unsigned long)total_kb, latest_name);
        }
    }

    return count;
}

int remote_sd_upload_file_sync(const char *filename, const char *server_url, const char *notify_phone)
{
    if (!filename || filename[0] == '\0') {
        LOGE("Upload target filename is empty!");
        return -1;
    }

    const char *url_to_use = (server_url && server_url[0] != '\0') ? server_url : s_configured_server_url;

    char host[96] = {0};
    uint16_t port = 80;
    char uri[128] = {0};
    if (!parse_http_url(url_to_use, host, sizeof(host), &port, uri, sizeof(uri))) {
        LOGE("Malformed server URL: '%s'", url_to_use);
        return -2;
    }

    char full_path[128];
    if (strncmp(filename, "0:/", 3) == 0 || strncmp(filename, "0:\\", 3) == 0) {
        strncpy(full_path, filename, sizeof(full_path) - 1);
    } else {
        snprintf(full_path, sizeof(full_path), "0:/%s", filename);
    }
    full_path[sizeof(full_path) - 1] = '\0';

    const char *base_filename = strrchr(full_path, '/');
    if (!base_filename) base_filename = strrchr(full_path, '\\');
    base_filename = base_filename ? (base_filename + 1) : full_path;

    /* Verify file exists and open it under SD lock */
    if (!nwy_sdcard_lock(5000)) {
        LOGE("Could not lock SD card for file open: '%s'", full_path);
        return -3;
    }
    FIL file_obj;
    FRESULT f_res = f_open(&file_obj, full_path, FA_READ);
    uint32_t file_size = 0;
    if (f_res == FR_OK) {
        file_size = (uint32_t)f_size(&file_obj);
    }
    nwy_sdcard_unlock();

    if (f_res != FR_OK) {
        if (f_res == 16 /* FR_LOCKED */) {
            LOGW("File '%s' is locked by an active recording session! Send 'STOP' before uploading.", base_filename);
            if (notify_phone && notify_phone[0] != '\0') {
                char alert[160];
                snprintf(alert, sizeof(alert), "UPLOAD PAUSED: '%s' is actively recording. Send 'STOP' first.", base_filename);
                nwy_hal_sms_send(1, notify_phone, alert);
            }
        } else {
            LOGE("Failed to open file '%s' on SD card! FatFs err: %d", full_path, f_res);
            if (notify_phone && notify_phone[0] != '\0') {
                char alert[160];
                snprintf(alert, sizeof(alert), "UPLOAD ERROR: File '%s' not found on MicroSD (err %d).", base_filename, f_res);
                nwy_hal_sms_send(1, notify_phone, alert);
            }
        }
        return -3;
    }

    s_total_bytes = file_size;
    s_bytes_sent = 0;
    s_upload_state = SD_UPLOAD_BUSY;

    LOGI("Preparing upload: '%s' (%lu bytes) -> %s:%u%s", base_filename, (unsigned long)file_size, host, port, uri);

    /* 1. Ensure Cellular Data Connection */
    if (!ensure_data_connection()) {
        nwy_sdcard_lock(5000);
        f_close(&file_obj);
        nwy_sdcard_unlock();
        s_upload_state = SD_UPLOAD_ERROR;
        if (notify_phone && notify_phone[0] != '\0') {
            nwy_hal_sms_send(1, notify_phone, "UPLOAD ERROR: Cellular LTE data connection unavailable.");
        }
        return -4;
    }

    /* 2. Resolve Server IP */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_len = sizeof(struct sockaddr_in);
    server_addr.sin_family = 2; // AF_INET
    server_addr.sin_port = nwy_socket_htons((int16_t)port);

    if (nwy_socket_inet_pton(2, host, &server_addr.sin_addr) != NWY_SUCCESS) {
        int isipv6 = 0;
        char *resolved_ip = nwy_socket_gethost_by_name(host, &isipv6, 1);
        if (!resolved_ip || strlen(resolved_ip) == 0 ||
            nwy_socket_inet_pton(2, resolved_ip, &server_addr.sin_addr) != NWY_SUCCESS) {
            LOGE("DNS resolution failed for host: '%s'", host);
            nwy_sdcard_lock(5000);
            f_close(&file_obj);
            nwy_sdcard_unlock();
            s_upload_state = SD_UPLOAD_ERROR;
            if (notify_phone && notify_phone[0] != '\0') {
                char alert[160];
                snprintf(alert, sizeof(alert), "UPLOAD ERROR: Could not resolve server '%s'.", host);
                nwy_hal_sms_send(1, notify_phone, alert);
            }
            return -5;
        }
        LOGI("DNS resolved '%s' -> %s", host, resolved_ip);
    }

    /* 3. Open TCP Socket */
    int sock = nwy_socket_open(2, 1, 6, 1); // AF_INET, SOCK_STREAM, TCP, CID 1
    if (sock < 0) {
        LOGE("Failed to allocate socket descriptor!");
        nwy_sdcard_lock(5000);
        f_close(&file_obj);
        nwy_sdcard_unlock();
        s_upload_state = SD_UPLOAD_ERROR;
        return -6;
    }

    /* Set socket Send and Receive timeouts (15 seconds) to prevent stalls */
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    nwy_socket_setsockopt(sock, 0xfff /* SOL_SOCKET */, 0x1006 /* SO_RCVTIMEO */, &tv, sizeof(tv));
    nwy_socket_setsockopt(sock, 0xfff /* SOL_SOCKET */, 0x1005 /* SO_SNDTIMEO */, &tv, sizeof(tv));

    /* 4. Connect to HTTP Server */
    LOGI("Connecting to %s:%u...", host, port);
    int conn_res = nwy_socket_connect(sock, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    if (conn_res != 0 && nwy_socket_get_state(sock) != NWY_ESTABLISHED) {
        /* Retry connection up to 3 times */
        bool connected = false;
        for (int r = 1; r <= 3; r++) {
            nwy_hal_os_thread_sleep(1000);
            if (nwy_socket_get_state(sock) == NWY_ESTABLISHED) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            LOGE("TCP connection to %s:%u failed! errno: %d", host, port, nwy_socket_errno());
            nwy_socket_close(sock);
            nwy_sdcard_lock(5000);
            f_close(&file_obj);
            nwy_sdcard_unlock();
            s_upload_state = SD_UPLOAD_ERROR;
            if (notify_phone && notify_phone[0] != '\0') {
                char alert[160];
                snprintf(alert, sizeof(alert), "UPLOAD ERROR: Failed to connect to %s:%u.", host, port);
                nwy_hal_sms_send(1, notify_phone, alert);
            }
            return -7;
        }
    }
    LOGI("TCP socket connected! Transmitting HTTP POST header...");

    /* 5. Transmit HTTP POST Header (with Tunnel Screen Bypass Headers) */
    char http_header[640];
    int hdr_len = snprintf(http_header, sizeof(http_header),
                           "POST %s?filename=%s HTTP/1.1\r\n"
                           "Host: %s:%u\r\n"
                           "User-Agent: Neoway-N706B/1.0\r\n"
                           "X-Pinggy-No-Screen: true\r\n"
                           "ngrok-skip-browser-warning: 69420\r\n"
                           "bypass-tunnel-reminder: 1\r\n"
                           "Content-Type: application/octet-stream\r\n"
                           "X-Filename: %s\r\n"
                           "Content-Length: %lu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           uri, base_filename, host, port, base_filename, (unsigned long)file_size);

    int sent_hdr = nwy_socket_send(sock, http_header, hdr_len, 0);
    if (sent_hdr < hdr_len) {
        LOGE("Failed to send full HTTP header (sent %d of %d)", sent_hdr, hdr_len);
        nwy_socket_close(sock);
        nwy_sdcard_lock(5000);
        f_close(&file_obj);
        nwy_sdcard_unlock();
        s_upload_state = SD_UPLOAD_ERROR;
        return -8;
    }

    /* 6. Stream File Binary Chunks into Socket */
    uint8_t chunk_buf[UPLOAD_CHUNK_SIZE];
    UINT bytes_read = 0;
    bool stream_ok = true;

    while (s_bytes_sent < file_size) {
        if (!nwy_sdcard_lock(5000)) {
            LOGE("Could not lock SD card for chunk read");
            stream_ok = false;
            break;
        }
        FRESULT fr = f_read(&file_obj, chunk_buf, sizeof(chunk_buf), &bytes_read);
        nwy_sdcard_unlock();

        if (fr != FR_OK || bytes_read == 0) {
            LOGE("Error reading from SD card! (fr: %d, bytes_read: %u)", fr, (unsigned int)bytes_read);
            stream_ok = false;
            break;
        }

        int chunk_sent = nwy_socket_send(sock, chunk_buf, bytes_read, 0);
        if (chunk_sent <= 0) {
            LOGE("Socket send error during stream! errno: %d", nwy_socket_errno());
            stream_ok = false;
            break;
        }

        s_bytes_sent += chunk_sent;
        if ((s_bytes_sent % (UPLOAD_CHUNK_SIZE * 5)) == 0 || s_bytes_sent == file_size) {
            uint32_t pct = (file_size > 0) ? ((s_bytes_sent * 100) / file_size) : 0;
            LOGI("Uploading '%s': %lu / %lu bytes (%u%%)", base_filename, (unsigned long)s_bytes_sent, (unsigned long)file_size, (unsigned int)pct);
        }
    }

    nwy_sdcard_lock(5000);
    f_close(&file_obj);
    nwy_sdcard_unlock();

    if (!stream_ok || s_bytes_sent < file_size) {
        LOGE("Stream interrupted! Sent %lu of %lu bytes.", (unsigned long)s_bytes_sent, (unsigned long)file_size);
        nwy_socket_close(sock);
        s_upload_state = SD_UPLOAD_ERROR;
        if (notify_phone && notify_phone[0] != '\0') {
            char alert[160];
            snprintf(alert, sizeof(alert), "UPLOAD ERROR: Transfer interrupted (%lu/%lu bytes).",
                     (unsigned long)s_bytes_sent, (unsigned long)file_size);
            nwy_hal_sms_send(1, notify_phone, alert);
        }
        return -9;
    }

    /* 7. Receive Server Response */
    LOGI("File stream complete (%lu bytes). Awaiting HTTP 200 response...", (unsigned long)s_bytes_sent);
    char rx_buf[256] = {0};
    int rx_bytes = nwy_socket_recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    nwy_socket_close(sock);

    bool upload_accepted = false;
    if (rx_bytes > 0) {
        rx_buf[rx_bytes] = '\0';
        LOGI("Server Response (%d bytes):\n%s", rx_bytes, rx_buf);
        if (strstr(rx_buf, "Pinggy") || strstr(rx_buf, "Caution") || strstr(rx_buf, "tunnel-reminder") ||
            strstr(rx_buf, "<html") || strstr(rx_buf, "<HTML") || strstr(rx_buf, "<!DOCTYPE") || strstr(rx_buf, "<!doctype")) {
            LOGE("REJECTED: Interrupted by proxy warning/HTML screening page! File was NOT received by server.");
            upload_accepted = false;
        } else if (strstr(rx_buf, "\"status\":\"ok\"") || strstr(rx_buf, "\"status\": \"ok\"") ||
                   strstr(rx_buf, "\"status\":\"OK\"") || strstr(rx_buf, "\"status\": \"OK\"")) {
            LOGI("Verified server acknowledgement: JSON status:ok confirmed!");
            upload_accepted = true;
        } else if ((strstr(rx_buf, "200 OK") || strstr(rx_buf, "201 Created") || strstr(rx_buf, "204 No Content")) &&
                   !strstr(rx_buf, "<")) {
            upload_accepted = true;
        } else {
            LOGE("REJECTED: Server did not return a valid JSON status:ok confirmation!");
            upload_accepted = false;
        }
    } else {
        LOGE("REJECTED: No response data received from server!");
        upload_accepted = false;
    }

    if (upload_accepted) {
        s_upload_state = SD_UPLOAD_SUCCESS;
        LOGI("SUCCESS: File '%s' (%lu bytes) uploaded to HTTP server!", base_filename, (unsigned long)file_size);
        if (notify_phone && notify_phone[0] != '\0') {
            char alert[160];
            snprintf(alert, sizeof(alert),
                     "UPLOAD OK: '%s' (%lu bytes) transferred to %s successfully.",
                     base_filename, (unsigned long)file_size, host);
            nwy_hal_sms_send(1, notify_phone, alert);
        }
        return 0;
    } else {
        s_upload_state = SD_UPLOAD_ERROR;
        LOGE("Server rejected upload or returned non-200 code!");
        if (notify_phone && notify_phone[0] != '\0') {
            char alert[160];
            snprintf(alert, sizeof(alert), "UPLOAD FAILED: Server rejected '%s'.", base_filename);
            nwy_hal_sms_send(1, notify_phone, alert);
        }
        return -10;
    }
}

int remote_sd_upload_latest_sync(const char *server_url, const char *notify_phone)
{
    DIR dir;
    FILINFO fno;
    char latest_raw[64] = {0};
    char latest_info[64] = {0};
    WORD latest_date = 0;
    WORD latest_time = 0;

    if (!nwy_sdcard_lock(5000)) {
        LOGE("Failed to acquire SD card mutex for upload latest!");
        return -1;
    }

    FRESULT res = f_opendir(&dir, "0:/");
    if (res != FR_OK) {
        nwy_sdcard_unlock();
        LOGE("Failed to open SD card root directory!");
        return -1;
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (!(fno.fattrib & AM_DIR)) {
            if (is_bmerawdata_ext(fno.fname)) {
                if (fno.fdate > latest_date || (fno.fdate == latest_date && fno.ftime >= latest_time)) {
                    latest_date = fno.fdate;
                    latest_time = fno.ftime;
                    strncpy(latest_raw, fno.fname, sizeof(latest_raw) - 1);
                }
            }
        }
    }
    f_closedir(&dir);
    nwy_sdcard_unlock();

    if (latest_raw[0] == '\0') {
        LOGE("No .bmerawdata file found on SD card!");
        if (notify_phone && notify_phone[0] != '\0') {
            nwy_hal_sms_send(1, notify_phone, "UPLOAD: No recorded datasets found on SD card.");
        }
        return -2;
    }

    /* Derive corresponding .bmelabelinfo filename */
    strncpy(latest_info, latest_raw, sizeof(latest_info) - 1);
    char *dot = strrchr(latest_info, '.');
    if (dot) strcpy(dot, ".bmelabelinfo");

    LOGI("Identified latest dataset session: '%s' and '%s'", latest_raw, latest_info);

    /* 1. Upload .bmerawdata */
    int ret1 = remote_sd_upload_file_sync(latest_raw, server_url, NULL);

    /* 2. Upload .bmelabelinfo if it exists */
    int ret2 = 0;
    bool info_exists = false;
    if (nwy_sdcard_lock(5000)) {
        FIL test_fil;
        if (f_open(&test_fil, latest_info, FA_READ) == FR_OK) {
            f_close(&test_fil);
            info_exists = true;
        }
        nwy_sdcard_unlock();
    }
    if (info_exists) {
        ret2 = remote_sd_upload_file_sync(latest_info, server_url, NULL);
    }

    if (notify_phone && notify_phone[0] != '\0') {
        char alert[160];
        if (ret1 == 0) {
            snprintf(alert, sizeof(alert),
                     "UPLOAD LATEST OK: Session '%s' uploaded successfully.", latest_raw);
        } else {
            snprintf(alert, sizeof(alert),
                     "UPLOAD LATEST FAILED: Error uploading '%s' (code %d).", latest_raw, ret1);
        }
        nwy_hal_sms_send(1, notify_phone, alert);
    }

    return (ret1 == 0 && ret2 == 0) ? 0 : -3;
}

int remote_sd_upload_all_sync(const char *server_url, const char *notify_phone)
{
    DIR dir;
    FILINFO fno;
    int uploaded_count = 0;
    int failed_count = 0;
    char file_list[32][64];
    int target_files = 0;

    if (!nwy_sdcard_lock(5000)) {
        LOGE("Could not lock SD card for batch scan");
        return -1;
    }

    FRESULT res = f_opendir(&dir, "0:/");
    if (res != FR_OK) {
        nwy_sdcard_unlock();
        return -1;
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0 && target_files < 32) {
        if (!(fno.fattrib & AM_DIR)) {
            if (has_dataset_extension(fno.fname)) {
                strncpy(file_list[target_files], fno.fname, sizeof(file_list[target_files]) - 1);
                file_list[target_files][sizeof(file_list[target_files]) - 1] = '\0';
                target_files++;
            }
        }
    }
    f_closedir(&dir);
    nwy_sdcard_unlock();

    LOGI("Starting batch upload of %d dataset files found on SD card...", target_files);

    for (int i = 0; i < target_files; i++) {
        LOGI("Batch upload [%d/%d]: uploading '%s'...", i + 1, target_files, file_list[i]);
        int ret = remote_sd_upload_file_sync(file_list[i], server_url, NULL);
        if (ret == 0) {
            uploaded_count++;
        } else {
            failed_count++;
        }
        nwy_hal_os_thread_sleep(500); // Inter-file breather
    }

    LOGI("Batch upload completed: %d uploaded, %d failed", uploaded_count, failed_count);

    if (notify_phone && notify_phone[0] != '\0') {
        char alert[160];
        snprintf(alert, sizeof(alert),
                 "UPLOAD ALL COMPLETE: %d uploaded, %d failed.", uploaded_count, failed_count);
        nwy_hal_sms_send(1, notify_phone, alert);
    }

    return uploaded_count;
}

/* -------------------------------------------------------------------------
 * ASYNCHRONOUS BACKGROUND UPLOAD WORKER TASK
 * ------------------------------------------------------------------------- */
static void remote_sd_uploader_task(void *param)
{
    (void)param;
    LOGI("Remote SD uploader worker thread active.");

    while (1) {
        nwy_hal_os_semaphore_acquire(s_upload_sem, 0xFFFFFFFF);

        if (s_has_pending_req) {
            sd_upload_request_t req;
            memcpy(&req, &s_pending_req, sizeof(req));
            s_has_pending_req = false;

            const char *url_to_use = (req.server_url[0] != '\0') ? req.server_url : s_configured_server_url;

            switch (req.target_type) {
            case UPLOAD_TARGET_SINGLE:
                LOGI("Executing queued SINGLE upload of '%s' to %s", req.filename, url_to_use);
                remote_sd_upload_file_sync(req.filename, url_to_use, req.notify_phone);
                break;

            case UPLOAD_TARGET_LATEST:
                LOGI("Executing queued LATEST upload to %s", url_to_use);
                remote_sd_upload_latest_sync(url_to_use, req.notify_phone);
                break;

            case UPLOAD_TARGET_ALL:
                LOGI("Executing queued ALL upload to %s", url_to_use);
                remote_sd_upload_all_sync(url_to_use, req.notify_phone);
                break;

            default:
                break;
            }
        }
    }
}

bool remote_sd_queue_upload(const sd_upload_request_t *req)
{
    if (!req) return false;
    if (s_upload_state == SD_UPLOAD_BUSY || s_has_pending_req) {
        LOGW("Cannot queue upload: uploader currently busy!");
        return false;
    }

    memcpy(&s_pending_req, req, sizeof(s_pending_req));
    s_has_pending_req = true;

    if (s_upload_sem) {
        nwy_hal_os_semaphore_release(s_upload_sem);
    }
    return true;
}

void remote_sd_get_upload_status(sd_upload_state_e *state_out, uint32_t *bytes_sent, uint32_t *total_bytes)
{
    if (state_out) *state_out = s_upload_state;
    if (bytes_sent) *bytes_sent = s_bytes_sent;
    if (total_bytes) *total_bytes = s_total_bytes;
}

int remote_sd_manager_init(void)
{
    if (s_upload_sem != NULL) {
        return 0; // Already initialized
    }

    nwy_hal_os_semaphore_create(&s_upload_sem, 0, 1);

    int th_ret = nwy_thread_create(&s_upload_thread, "sd_uploader", 48,
                                   remote_sd_uploader_task, NULL, 10, 1024 * 16, NULL);
    if (th_ret != 0) {
        LOGE("Failed to spawn SD uploader worker thread!");
        return -1;
    }

    LOGI("Remote SD Card Manager initialized successfully.");
    return 0;
}
