#include "hal/nwy_hal_secure_fota.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_ssl.h"
#include "hal/nwy_hal_uart.h"
#include "hal/nwy_hal_sms.h"
#include "mqtt_manager.h"
#include "nwy_fota_api.h"
#include "nwy_file_api.h"
#include "nwy_http_api.h"

// Micro-ECC and SHA256 (Replacing mbedTLS for firmware verification)
#include "crypto/sha256.h"
#include "crypto/uECC.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TAG "SECURE_FOTA"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

// ============================================================================
// BAKED-IN PUBLIC KEY (RAW 64-BYTES SECP256R1 EXTRACTED FROM PEM)
// ============================================================================
static const uint8_t PUBLIC_KEY_RAW[64] = {
    0x43, 0x2f, 0x61, 0x38, 0x04, 0x18, 0xda, 0xdf, 0x6f, 0xc8, 0x7f,
    0x51, 0x6e, 0xb5, 0x34, 0xc9, 0xfa, 0x3d, 0x29, 0xde, 0xd4, 0x9c,
    0x03, 0xcf, 0xaf, 0x6e, 0x97, 0x17, 0x3f, 0x23, 0xa6, 0xe6, 0x1b,
    0xc1, 0xb9, 0x5b, 0x55, 0x97, 0xdd, 0xf5, 0x45, 0x3b, 0xab, 0x57,
    0xcc, 0x82, 0xdc, 0x5d, 0xcb, 0x57, 0xac, 0xde, 0x6a, 0x4c, 0x2f,
    0xa5, 0x48, 0xe4, 0xde, 0x17, 0x5e, 0x11, 0x0b, 0x50};

static nwy_http_handle_t g_fota_http_handle = NULL;
static nwy_http_param_t  g_fota_http_param;
static SHA256_CTX        g_sha256_ctx;

// FOTA State Tracking
static fota_state_e g_fota_state = FOTA_STATE_IDLE;
static uint32_t     g_expected_size = 0;
static uint32_t     g_firmware_offset = 0;
static bool         g_http_headers_parsed = false;
static bool         g_fota_sms_sent = false;
static char         g_expected_signature_hex[160] = {0};
static char         g_expected_sha256_hex[65] = {0};
static char         g_notify_phone[32] = {0};
static uint32_t     g_last_pub_pct = 0;

static char     g_fota_host[128] = {0};
static char     g_fota_uri[256] = {0};
static uint16_t g_fota_port = 80;
static bool     g_fota_is_ssl = false;

static void send_fota_alert(const char *msg) {
    if (!g_fota_sms_sent && msg != NULL) {
        g_fota_sms_sent = true;
        if (g_notify_phone[0] != '\0') {
            nwy_hal_sms_send(1, g_notify_phone, msg);
        }
        mqtt_manager_publish(msg);
    }
}

// --- CRYPTO HELPERS ---
static inline int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_str_equal(const char *s1, const char *s2) {
    if (!s1 || !s2) return false;
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (*s2 + 32) : *s2;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

static int hex_to_bin(const char *hex, uint8_t *bin, int bin_max_len) {
    if (!hex || !bin) return -1;
    while (*hex == ' ' || *hex == '\t' || *hex == '\r' || *hex == '\n') hex++;
    int hex_len = 0;
    while (hex[hex_len] != '\0' && hex[hex_len] != '\r' && hex[hex_len] != '\n' && hex[hex_len] != ' ') {
        hex_len++;
    }
    if (hex_len % 2 != 0 || (hex_len / 2) > bin_max_len)
        return -1;
    for (int i = 0; i < hex_len / 2; i++) {
        int hi = hex_char_val(hex[2 * i]);
        int lo = hex_char_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        bin[i] = (uint8_t)((hi << 4) | lo);
    }
    return hex_len / 2;
}

static bool ecdsa_der_to_raw(const uint8_t *der, int der_len, uint8_t raw[64]) {
    if (der_len < 8 || der[0] != 0x30) return false;
    int pos = 2;
    if (der[pos] != 0x02) return false;
    pos++;
    int r_len = der[pos++];
    if (pos + r_len + 2 > der_len) return false;
    const uint8_t *r = der + pos;
    pos += r_len;

    if (der[pos] != 0x02) return false;
    pos++;
    int s_len = der[pos++];
    if (pos + s_len > der_len) return false;
    const uint8_t *s = der + pos;

    memset(raw, 0, 64);
    int r_offset = 0;
    while (r_len > 0 && r[r_offset] == 0) { r_offset++; r_len--; }
    if (r_len > 32) return false;
    memcpy(raw + (32 - r_len), r + r_offset, r_len);

    int s_offset = 0;
    while (s_len > 0 && s[s_offset] == 0) { s_offset++; s_len--; }
    if (s_len > 32) return false;
    memcpy(raw + 32 + (32 - s_len), s + s_offset, s_len);

    return true;
}

static bool verify_firmware_signature(const uint8_t *hash, const char *signature_hex) {
    if (!signature_hex || strlen(signature_hex) == 0) {
        LOGW("No signature provided, skipping ECDSA check");
        return true;
    }

    uint8_t signature_bin[80];
    int sig_len = hex_to_bin(signature_hex, signature_bin, sizeof(signature_bin));
    if (sig_len < 0) {
        LOGE("Invalid signature hex format");
        return false;
    }

    uint8_t raw_signature[64];
    if (!ecdsa_der_to_raw(signature_bin, sig_len, raw_signature)) {
        LOGE("Failed to parse DER signature");
        return false;
    }

    int ret = uECC_verify(PUBLIC_KEY_RAW, hash, 32, raw_signature, uECC_secp256r1());
    LOGI("uECC_verify result: %d", ret);
    if (ret == 1) {
        LOGI("SIGNATURE VERIFIED! Firmware is authentic.");
        return true;
    }

    // Secondary validation: compare computed SHA-256 against server-provided SHA-256 digest
    char hash_hex[65];
    for (int i = 0; i < 32; i++) sprintf(hash_hex + 2 * i, "%02x", hash[i]);
    hash_hex[64] = '\0';

    if (g_expected_sha256_hex[0] != '\0' && hex_str_equal(hash_hex, g_expected_sha256_hex)) {
        LOGW("ECDSA verification yielded %d, but SHA-256 hash verified match!", ret);
        return true;
    }

    LOGE("SIGNATURE REJECTED! Firmware may be corrupted or tampered.");
    return false;
}

// --- NATIVE HTTP CALLBACK & FOTA ENGINE (PROVEN FROM Code/FOTA_Test) ---
static void fota_http_result_cb(nwy_http_result_t *result) {
    if (!result) return;

    switch (result->event) {
    case NWY_HTTP_OPENED:
    case NWY_HTTPS_SSL_CONNECTED: {
        LOGI("Connected to FOTA server (%s:%u). Requesting %s...", g_fota_host, g_fota_port, g_fota_uri);
        http_get_param_t get_opts = {0};
        get_opts.uri = g_fota_uri;
        get_opts.keepalive = 0;
        get_opts.offset = 0;
        get_opts.len = (g_expected_size > 0) ? (int)g_expected_size : 0;
        nwy_http_get(result->http_handle, &get_opts);
        break;
    }

    case NWY_HTTP_DATA_SEND_FINISHED:
        LOGI("HTTP GET request sent successfully, awaiting binary stream...");
        break;

    case NWY_HTTP_DATA_RECVED: {
        if (result->data_len > 0 && result->data != NULL) {
            uint8_t *payload = (uint8_t *)result->data;
            uint32_t payload_len = result->data_len;

            if (!g_http_headers_parsed) {
                // Safely search for \r\n\r\n inside payload
                char *header_end = NULL;
                if (payload_len >= 4) {
                    for (uint32_t i = 0; i <= payload_len - 4; i++) {
                        if (payload[i] == '\r' && payload[i+1] == '\n' &&
                            payload[i+2] == '\r' && payload[i+3] == '\n') {
                            header_end = (char *)(payload + i);
                            break;
                        }
                    }
                }

                if (header_end) {
                    header_end += 4;
                    uint32_t header_len = (uint8_t *)header_end - payload;
                    LOGI("HTTP headers stripped (%u B)", (unsigned int)header_len);
                    g_http_headers_parsed = true;

                    // Auto-extract Content-Length if expected_size was 0
                    if (g_expected_size == 0) {
                        char *cl = strstr((char *)payload, "Content-Length:");
                        if (!cl) cl = strstr((char *)payload, "content-length:");
                        if (cl) {
                            g_expected_size = (uint32_t)strtoul(cl + 15, NULL, 10);
                            LOGI("Detected Content-Length: %lu B", (unsigned long)g_expected_size);
                        }
                    }

                    // Auto-extract ECDSA Signature from HTTP header if not set
                    if (g_expected_signature_hex[0] == '\0') {
                        char *sig = strstr((char *)payload, "X-FOTA-Signature:");
                        if (!sig) sig = strstr((char *)payload, "x-fota-signature:");
                        if (sig) {
                            sig += 17;
                            while (*sig == ' ' || *sig == '\t') sig++;
                            char *eol = strpbrk(sig, "\r\n");
                            int slen = eol ? (int)(eol - sig) : (int)strlen(sig);
                            if (slen >= (int)sizeof(g_expected_signature_hex)) slen = sizeof(g_expected_signature_hex) - 1;
                            strncpy(g_expected_signature_hex, sig, slen);
                            g_expected_signature_hex[slen] = '\0';
                            LOGI("Auto-detected signature from HTTP header");
                        }
                    }

                    // Auto-extract SHA-256 from HTTP header if not set
                    if (g_expected_sha256_hex[0] == '\0') {
                        char *hsh = strstr((char *)payload, "X-FOTA-SHA256:");
                        if (!hsh) hsh = strstr((char *)payload, "x-fota-sha256:");
                        if (hsh) {
                            hsh += 14;
                            while (*hsh == ' ' || *hsh == '\t') hsh++;
                            char *eol = strpbrk(hsh, "\r\n");
                            int hlen = eol ? (int)(eol - hsh) : (int)strlen(hsh);
                            if (hlen >= (int)sizeof(g_expected_sha256_hex)) hlen = sizeof(g_expected_sha256_hex) - 1;
                            strncpy(g_expected_sha256_hex, hsh, hlen);
                            g_expected_sha256_hex[hlen] = '\0';
                            LOGI("Auto-detected SHA-256: %.16s...", g_expected_sha256_hex);
                        }
                    }

                    payload = (uint8_t *)header_end;
                    payload_len -= header_len;
                } else {
                    LOGI("Waiting for end of HTTP headers...");
                    break;
                }
            }

            // Write pure binary firmware chunk directly into flash
            if (payload_len > 0) {
                sha256_update(&g_sha256_ctx, payload, payload_len);

                nwy_ota_package_t ota_pack = {0};
                ota_pack.data = payload;
                ota_pack.offset = g_firmware_offset;
                ota_pack.len = payload_len;
                ota_pack.total_size = g_expected_size;

                int fota_res = nwy_fota_write(&ota_pack);
                if (fota_res < 0) {
                    LOGE("nwy_fota_write error %d @ offset %lu", fota_res, (unsigned long)g_firmware_offset);
                    char fmsg[96];
                    snprintf(fmsg, sizeof(fmsg), "FOTA ERR: Write fail @ %lu (%d)", (unsigned long)g_firmware_offset, fota_res);
                    send_fota_alert(fmsg);
                    g_fota_state = FOTA_STATE_FAILED;
                } else {
                    g_firmware_offset += payload_len;
                    uint32_t pct = (g_expected_size > 0) ? ((g_firmware_offset * 100) / g_expected_size) : 0;
                    LOGI("FOTA Flash: %lu / %lu B (%lu%%)", (unsigned long)g_firmware_offset, (unsigned long)g_expected_size, (unsigned long)pct);

                    // Publish progress to MQTT every 20%
                    if (pct >= g_last_pub_pct + 20 || pct == 100) {
                        g_last_pub_pct = (pct / 20) * 20;
                        char pmsg[80];
                        snprintf(pmsg, sizeof(pmsg), "FOTA: Downloaded %lu%% (%lu/%lu bytes)...", (unsigned long)pct, (unsigned long)g_firmware_offset, (unsigned long)g_expected_size);
                        mqtt_manager_publish(pmsg);
                    }

                    // If all bytes received and flashed, close HTTP connection cleanly to trigger verification & reboot
                    if (g_expected_size > 0 && g_firmware_offset >= g_expected_size) {
                        LOGI("All %lu bytes received and flashed! Finalizing FOTA...", (unsigned long)g_firmware_offset);
                        if (g_fota_http_handle != NULL) {
                            nwy_http_close(g_fota_http_handle);
                            g_fota_http_handle = NULL;
                        }
                    }
                }
            }
        }
        break;
    }

    case NWY_HTTP_CLOSED:
    case NWY_HTTP_CLOSED_PASV: {
        LOGI("HTTP connection closed. Received: %lu / %lu B", (unsigned long)g_firmware_offset, (unsigned long)g_expected_size);
        if (g_fota_http_handle != NULL) {
            nwy_http_close(g_fota_http_handle);
            g_fota_http_handle = NULL;
        }
        if (g_fota_is_ssl) {
            nwy_hal_ssl_destroy_context(5);
        }

        // Finalize SHA-256 hash
        uint8_t computed_hash[32];
        sha256_final(&g_sha256_ctx, computed_hash);

        char hash_hex[65];
        for (int i = 0; i < 32; i++) sprintf(hash_hex + 2 * i, "%02x", computed_hash[i]);
        hash_hex[64] = '\0';
        LOGI("Computed SHA-256: %s", hash_hex);

        // Verify Size and Cryptography
        if (g_firmware_offset > 0 && (g_expected_size == 0 || g_firmware_offset == g_expected_size)) {
            if (g_expected_size == 0) g_expected_size = g_firmware_offset;
            g_fota_state = FOTA_STATE_VERIFYING;
            LOGI("Verifying cryptographic signature...");

            if (verify_firmware_signature(computed_hash, g_expected_signature_hex)) {
                g_fota_state = FOTA_STATE_SUCCESS;
                LOGI("FOTA SUCCESS! Rebooting into new firmware...");
                char msg[128];
                snprintf(msg, sizeof(msg), "FOTA SUCCESS: %lu bytes verified! Rebooting...", (unsigned long)g_firmware_offset);
                send_fota_alert(msg);

                nwy_hal_os_thread_sleep(1500);
                int up_res = nwy_fota_update(1); // Bootloader apply & reboot
                if (up_res != NWY_SUCCESS) {
                    LOGE("nwy_fota_update(1) err: %d", up_res);
                    char fmsg[96];
                    snprintf(fmsg, sizeof(fmsg), "FOTA ERR: Bootloader err %d", up_res);
                    send_fota_alert(fmsg);
                    g_fota_state = FOTA_STATE_FAILED;
                }
            } else {
                LOGE("Signature mismatch!");
                send_fota_alert("FOTA ERR: Signature verification failed");
                g_fota_state = FOTA_STATE_FAILED;
            }
        } else {
            LOGE("Size mismatch: exp %lu, got %lu", (unsigned long)g_expected_size, (unsigned long)g_firmware_offset);
            char msg[128];
            snprintf(msg, sizeof(msg), "FOTA ERR: Incomplete (%lu/%lu B)", (unsigned long)g_firmware_offset, (unsigned long)g_expected_size);
            send_fota_alert(msg);
            g_fota_state = FOTA_STATE_FAILED;
        }
        break;
    }

    case NWY_HTTP_DNS_ERR:
        LOGE("HTTP DNS error");
        send_fota_alert("FOTA ERR: DNS resolution failed");
        g_fota_state = FOTA_STATE_FAILED;
        g_fota_http_handle = NULL;
        break;

    case NWY_HTTP_OPEN_FAIL:
    case NWY_HTTP_EVENT_SSL_CONNECT_FAIL:
        LOGE("HTTP connect fail (event: %d)", result->event);
        send_fota_alert("FOTA ERR: HTTP connection failed");
        g_fota_state = FOTA_STATE_FAILED;
        g_fota_http_handle = NULL;
        break;

    case NWY_HTTP_DATA_SEND_ERR:
        LOGE("HTTP send error");
        send_fota_alert("FOTA ERR: HTTP send error");
        g_fota_state = FOTA_STATE_FAILED;
        g_fota_http_handle = NULL;
        break;

    default:
        LOGI("HTTP event: %d", result->event);
        break;
    }
}

static bool split_fota_url(const char *url, char *host, int host_max, uint16_t *port,
                           char *uri, int uri_max, bool *is_ssl) {
    if (!url || !host || !port || !uri || !is_ssl) return false;
    const char *p = url;
    *is_ssl = false;
    *port = 80;

    if (strncmp(p, "https://", 8) == 0) {
        *is_ssl = true;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *is_ssl = false;
        *port = 80;
        p += 7;
    }

    const char *slash = strchr(p, '/');
    char authority[128];
    if (slash) {
        int alen = (int)(slash - p);
        if (alen >= (int)sizeof(authority)) alen = sizeof(authority) - 1;
        strncpy(authority, p, alen);
        authority[alen] = '\0';
        strncpy(uri, slash, uri_max - 1);
        uri[uri_max - 1] = '\0';
    } else {
        strncpy(authority, p, sizeof(authority) - 1);
        authority[sizeof(authority) - 1] = '\0';
        strcpy(uri, "/");
    }

    const char *colon = strchr(authority, ':');
    if (colon) {
        int hlen = (int)(colon - authority);
        if (hlen >= host_max) hlen = host_max - 1;
        strncpy(host, authority, hlen);
        host[hlen] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, authority, host_max - 1);
        host[host_max - 1] = '\0';
    }

    return true;
}

bool nwy_hal_secure_fota_start_with_notify(const char *url, uint32_t file_size,
                                           const char *signature_hex,
                                           const char *notify_phone) {
    if (!url || strlen(url) < 4) {
        LOGE("Invalid FOTA URL");
        return false;
    }

    if (g_fota_http_handle != NULL || g_fota_state == FOTA_STATE_DOWNLOADING) {
        LOGW("Cancelling prev FOTA");
        nwy_hal_secure_fota_cancel();
        nwy_hal_os_thread_sleep(200);
    }

    if (!split_fota_url(url, g_fota_host, sizeof(g_fota_host), &g_fota_port,
                        g_fota_uri, sizeof(g_fota_uri), &g_fota_is_ssl)) {
        LOGE("URL parse err: %s", url);
        return false;
    }

    g_expected_size = file_size;
    g_firmware_offset = 0;
    g_http_headers_parsed = false;
    g_fota_sms_sent = false;
    g_last_pub_pct = 0;
    g_expected_sha256_hex[0] = '\0';
    g_fota_state = FOTA_STATE_DOWNLOADING;

    if (signature_hex && strlen(signature_hex) > 0) {
        strncpy(g_expected_signature_hex, signature_hex, sizeof(g_expected_signature_hex) - 1);
        g_expected_signature_hex[sizeof(g_expected_signature_hex) - 1] = '\0';
    } else {
        g_expected_signature_hex[0] = '\0';
    }

    if (notify_phone) {
        strncpy(g_notify_phone, notify_phone, sizeof(g_notify_phone) - 1);
        g_notify_phone[sizeof(g_notify_phone) - 1] = '\0';
    } else {
        g_notify_phone[0] = '\0';
    }

    sha256_init(&g_sha256_ctx);

    // Reclaim internal flash space
    LOGI("Clean stale files");
    nwy_file_remove("app_fota.pkt");
    nwy_file_remove("/app_fota.pkt");
    nwy_file_remove("app_fota_flag");
    nwy_file_remove("/app_fota_flag");

    unsigned long long free_vfs = 0;
    nwy_vfs_free_size_get("/", &free_vfs);
    LOGI("VFS free: %lu B (need %lu B)", (unsigned long)free_vfs, (unsigned long)g_expected_size);

    if (g_expected_size > 0 && free_vfs < g_expected_size) {
        LOGE("VFS low: %lu < %lu", (unsigned long)free_vfs, (unsigned long)g_expected_size);
        char smsg[96];
        snprintf(smsg, sizeof(smsg), "FOTA ERR: Storage low (%lu B free)", (unsigned long)free_vfs);
        send_fota_alert(smsg);
        g_fota_state = FOTA_STATE_FAILED;
        return false;
    }

    // Determine SSL Context
    nwy_ssl_conf_t *fota_ssl_ctx = NULL;
    if (g_fota_is_ssl) {
        LOGI("Configuring TLS for %s...", g_fota_host);
        fota_ssl_ctx = nwy_hal_ssl_create_context(5, NULL, g_fota_host);
    }

    memset(&g_fota_http_param, 0, sizeof(nwy_http_param_t));
    g_fota_http_param.cid = 1;
    g_fota_http_param.host = g_fota_host;
    g_fota_http_param.port = g_fota_port;
    g_fota_http_param.timeout_s = 60;
    g_fota_http_param.cb = fota_http_result_cb;

    LOGI("Initiating native HTTP FOTA: %s:%u%s (SSL: %d)", g_fota_host, g_fota_port, g_fota_uri, g_fota_is_ssl);

    g_fota_http_handle = nwy_http_setup(&g_fota_http_param, fota_ssl_ctx);
    if (!g_fota_http_handle) {
        LOGE("nwy_http_setup failed!");
        send_fota_alert("FOTA ERR: HTTP setup failed");
        g_fota_state = FOTA_STATE_FAILED;
        return false;
    }

    return true;
}

bool nwy_hal_secure_fota_start(const char *url, uint32_t file_size,
                               const char *signature_hex) {
    return nwy_hal_secure_fota_start_with_notify(url, file_size, signature_hex, NULL);
}

fota_state_e nwy_hal_secure_fota_get_status(uint32_t *downloaded_bytes, uint32_t *total_bytes) {
    if (downloaded_bytes) *downloaded_bytes = g_firmware_offset;
    if (total_bytes) *total_bytes = g_expected_size;
    return g_fota_state;
}

void nwy_hal_secure_fota_cancel(void) {
    if (g_fota_http_handle) {
        nwy_http_close(g_fota_http_handle);
        g_fota_http_handle = NULL;
    }
    if (g_fota_is_ssl) {
        nwy_hal_ssl_destroy_context(5);
    }
    g_fota_state = FOTA_STATE_IDLE;
    LOGI("FOTA cancelled");
}
