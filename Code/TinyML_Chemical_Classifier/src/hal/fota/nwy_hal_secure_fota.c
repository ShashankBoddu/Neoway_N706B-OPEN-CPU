#include "hal/nwy_hal_secure_fota.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/nwy_hal_ssl.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/sms/nwy_hal_sms.h"
#include "nwy_fota_api.h"
#include "nwy_http_api.h"

// Micro-ECC and SHA256 (for cryptographic firmware verification)
#include "crypto/sha256.h"
#include "crypto/uECC.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SECURE_FOTA"

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

// ============================================================================
// BAKED-IN PUBLIC KEY (RAW 64-BYTES SECP256R1 EXTRACTED FROM fota_private_key.pem)
// ============================================================================
static const uint8_t PUBLIC_KEY_RAW[64] = {
    0x43, 0x2f, 0x61, 0x38, 0x04, 0x18, 0xda, 0xdf, 0x6f, 0xc8, 0x7f,
    0x51, 0x6e, 0xb5, 0x34, 0xc9, 0xfa, 0x3d, 0x29, 0xde, 0xd4, 0x9c,
    0x03, 0xcf, 0xaf, 0x6e, 0x97, 0x17, 0x3f, 0x23, 0xa6, 0xe6, 0x1b,
    0xc1, 0xb9, 0x5b, 0x55, 0x97, 0xdd, 0xf5, 0x45, 0x3b, 0xab, 0x57,
    0xcc, 0x82, 0xdc, 0x5d, 0xcb, 0x57, 0xac, 0xde, 0x6a, 0x4c, 0x2f,
    0xa5, 0x48, 0xe4, 0xde, 0x17, 0x5e, 0x11, 0x0b, 0x50};

static nwy_http_handle_t g_fota_http_handle = NULL;
static nwy_http_param_t g_fota_http_param;
static SHA256_CTX g_sha256_ctx;

// FOTA State Tracking
static fota_state_e g_fota_state = FOTA_STATE_IDLE;
static uint32_t g_expected_size = 0;
static uint32_t g_firmware_offset = 0;
static bool g_http_headers_parsed = false;
static char g_expected_signature_hex[160] = {0};
static char g_notify_phone[32] = {0};

static char g_fota_host[128] = {0};
static char g_fota_uri[256] = {0};

// --- CRYPTO HELPERS ---
static int hex_to_bin(const char *hex, uint8_t *bin, int bin_max_len) {
    int hex_len = strlen(hex);
    if (hex_len % 2 != 0 || (hex_len / 2) > bin_max_len)
        return -1;
    for (int i = 0; i < hex_len / 2; i++) {
        unsigned int byte_val = 0;
        if (sscanf(hex + 2 * i, "%02x", &byte_val) != 1) {
            return -1;
        }
        bin[i] = (uint8_t)byte_val;
    }
    return hex_len / 2;
}

static bool ecdsa_der_to_raw(const uint8_t *der, int der_len, uint8_t raw[64]) {
    if (der_len < 8 || der[0] != 0x30)
        return false;
    int pos = 2;
    if (der[pos] != 0x02)
        return false;
    pos++;
    int r_len = der[pos++];
    if (pos + r_len + 2 > der_len)
        return false;
    const uint8_t *r = der + pos;
    pos += r_len;

    if (der[pos] != 0x02)
        return false;
    pos++;
    int s_len = der[pos++];
    if (pos + s_len > der_len)
        return false;
    const uint8_t *s = der + pos;

    memset(raw, 0, 64);
    int r_offset = 0;
    while (r_len > 0 && r[r_offset] == 0) {
        r_offset++;
        r_len--;
    }
    if (r_len > 32)
        return false;
    memcpy(raw + (32 - r_len), r + r_offset, r_len);

    int s_offset = 0;
    while (s_len > 0 && s[s_offset] == 0) {
        s_offset++;
        s_len--;
    }
    if (s_len > 32)
        return false;
    memcpy(raw + 32 + (32 - s_len), s + s_offset, s_len);

    return true;
}

static bool verify_firmware_signature(const uint8_t *hash,
                                      const char *signature_hex) {
    uint8_t signature_bin[80];
    int sig_len = hex_to_bin(signature_hex, signature_bin, sizeof(signature_bin));
    if (sig_len < 0) {
        LOGE("Invalid signature hex format: len=%d", sig_len);
        return false;
    }

    uint8_t raw_signature[64];
    if (!ecdsa_der_to_raw(signature_bin, sig_len, raw_signature)) {
        LOGE("Failed to parse DER signature into raw coordinates");
        return false;
    }

    int ret = uECC_verify(PUBLIC_KEY_RAW, hash, 32, raw_signature, uECC_secp256r1());
    if (ret != 1) {
        LOGE("CRITICAL: ECDSA SECP256R1 SIGNATURE REJECTED! Corrupted or unauthorized binary.");
        return false;
    }

    LOGI("SUCCESS: Cryptographic signature verified! Firmware integrity confirmed.");
    return true;
}

// --- HTTP CALLBACK & FOTA ENGINE ---
static void fota_http_result_cb(nwy_http_result_t *result) {
    if (!result)
        return;

    switch (result->event) {
    case NWY_HTTP_OPENED:
    case NWY_HTTPS_SSL_CONNECTED: {
        LOGI("Connected to FOTA Server. Requesting binary package '%s'...", g_fota_uri);
        g_fota_state = FOTA_STATE_DOWNLOADING;
        http_get_param_t get_opts;
        memset(&get_opts, 0, sizeof(get_opts));
        get_opts.uri = g_fota_uri;
        get_opts.keepalive = 0;
        get_opts.offset = 0;
        get_opts.len = 0;
        nwy_http_get(result->http_handle, &get_opts);
        break;
    }

    case NWY_HTTP_DATA_RECVED: {
        if (result->data_len > 0 && result->data != NULL) {
            uint8_t *payload = (uint8_t *)result->data;
            uint32_t payload_len = result->data_len;

            if (!g_http_headers_parsed) {
                char *header_end = strstr((char *)payload, "\r\n\r\n");
                if (header_end) {
                    header_end += 4;
                    uint32_t header_len = (uint8_t *)header_end - payload;
                    LOGI("HTTP Headers Parsed. Stripping %u bytes of transport headers.", header_len);
                    g_http_headers_parsed = true;
                    payload = (uint8_t *)header_end;
                    payload_len -= header_len;
                } else {
                    LOGI("Waiting for end of HTTP headers...");
                    break;
                }
            }

            // Stream pure binary firmware data into flash & sha256
            if (payload_len > 0) {
                sha256_update(&g_sha256_ctx, payload, payload_len);

                nwy_ota_package_t ota_pack;
                memset(&ota_pack, 0, sizeof(ota_pack));
                ota_pack.data = payload;
                ota_pack.offset = g_firmware_offset;
                ota_pack.len = payload_len;
                ota_pack.total_size = g_expected_size;

                int fota_res = nwy_fota_write(&ota_pack);
                if (fota_res < 0) {
                    LOGE("nwy_fota_write REJECTED chunk! Error: %d.", fota_res);
                } else {
                    g_firmware_offset += payload_len;
                    uint32_t pct = (g_expected_size > 0) ? ((g_firmware_offset * 100) / g_expected_size) : 0;
                    LOGI("FOTA Flash Progress: %u / %u bytes (%u%%)", g_firmware_offset, g_expected_size, pct);
                }
            }
        }
        break;
    }

    case NWY_HTTP_CLOSED:
    case NWY_HTTP_CLOSED_PASV: {
        LOGW("FOTA connection closed. Downloaded: %u / %u bytes", g_firmware_offset, g_expected_size);
        nwy_http_close(result->http_handle);
        g_fota_http_handle = NULL;
        nwy_hal_ssl_destroy_context(5);

        // Finalize SHA-256 hash
        uint8_t computed_hash[32];
        sha256_final(&g_sha256_ctx, computed_hash);

        g_fota_state = FOTA_STATE_VERIFYING;

        if (g_firmware_offset > 0 && g_firmware_offset == g_expected_size) {
            LOGI("Download complete. Verifying ECDSA secp256r1 signature against SHA-256 hash...");
            if (verify_firmware_signature(computed_hash, g_expected_signature_hex)) {
                g_fota_state = FOTA_STATE_SUCCESS;
                LOGI("==========================================================");
                LOGI("FOTA UPDATE READY: Authentic image loaded into flash!");
                LOGI("Executing system reboot to apply update in bootloader...");
                LOGI("==========================================================");

                if (g_notify_phone[0] != '\0') {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "FOTA SUCCESS: %u bytes verified! Rebooting module into new firmware...",
                             (unsigned int)g_firmware_offset);
                    nwy_hal_sms_send(1, g_notify_phone, msg);
                }

                nwy_thread_sleep(2000);
                nwy_fota_update(1); // Notify bootloader and reboot
            } else {
                g_fota_state = FOTA_STATE_FAILED;
                LOGE("CRITICAL: Firmware signature verification failed! Update rejected.");
                if (g_notify_phone[0] != '\0') {
                    nwy_hal_sms_send(1, g_notify_phone, "FOTA ERROR: Signature verification failed. Update discarded.");
                }
            }
        } else {
            g_fota_state = FOTA_STATE_FAILED;
            LOGE("CRITICAL: FOTA size mismatch! Expected %u, got %u.", g_expected_size, g_firmware_offset);
            if (g_notify_phone[0] != '\0') {
                char msg[160];
                snprintf(msg, sizeof(msg), "FOTA ERROR: Download incomplete (%u/%u bytes). Update aborted.",
                         (unsigned int)g_firmware_offset, (unsigned int)g_expected_size);
                nwy_hal_sms_send(1, g_notify_phone, msg);
            }
        }
        break;
    }

    case NWY_HTTP_DNS_ERR:
    case NWY_HTTP_OPEN_FAIL:
    case NWY_HTTP_DATA_SEND_ERR:
    case NWY_HTTP_EVENT_SSL_CONNECT_FAIL:
    case NWY_HTTP_EVENT_UNKOWN: {
        LOGE("FOTA HTTP Transport Error (event: %d)!", result->event);
        if (result->http_handle) {
            nwy_http_close(result->http_handle);
        }
        g_fota_http_handle = NULL;
        nwy_hal_ssl_destroy_context(5);
        g_fota_state = FOTA_STATE_FAILED;
        if (g_notify_phone[0] != '\0') {
            char fail_msg[160];
            snprintf(fail_msg, sizeof(fail_msg),
                     "FOTA ERROR: Connection failed (code %d). Update aborted.", result->event);
            nwy_hal_sms_send(1, g_notify_phone, fail_msg);
        }
        break;
    }

    default:
        LOGI("Unhandled HTTP Event: %d", result->event);
        break;
    }
}

static void split_url(const char *url, char *host, int host_max, char *uri, int uri_max) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0)
        p += 8;
    else if (strncmp(p, "http://", 7) == 0)
        p += 7;

    const char *slash = strchr(p, '/');
    if (slash) {
        int len = slash - p;
        if (len >= host_max)
            len = host_max - 1;
        strncpy(host, p, len);
        host[len] = '\0';
        strncpy(uri, slash, uri_max - 1);
        uri[uri_max - 1] = '\0';
    } else {
        strncpy(host, p, host_max - 1);
        host[host_max - 1] = '\0';
        strcpy(uri, "/");
    }
}

bool nwy_hal_secure_fota_start_with_notify(const char *url, uint32_t file_size,
                                           const char *signature_hex,
                                           const char *notify_phone) {
    if (!url || file_size == 0 || !signature_hex) {
        LOGE("Invalid FOTA start parameters");
        return false;
    }

    if (g_fota_http_handle != NULL || g_fota_state == FOTA_STATE_DOWNLOADING) {
        LOGW("FOTA already in progress!");
        return false;
    }

    g_expected_size = file_size;
    g_firmware_offset = 0;
    g_http_headers_parsed = false;
    g_fota_state = FOTA_STATE_DOWNLOADING;

    strncpy(g_expected_signature_hex, signature_hex, sizeof(g_expected_signature_hex) - 1);
    g_expected_signature_hex[sizeof(g_expected_signature_hex) - 1] = '\0';

    if (notify_phone) {
        strncpy(g_notify_phone, notify_phone, sizeof(g_notify_phone) - 1);
        g_notify_phone[sizeof(g_notify_phone) - 1] = '\0';
    } else {
        g_notify_phone[0] = '\0';
    }

    sha256_init(&g_sha256_ctx);

    split_url(url, g_fota_host, sizeof(g_fota_host), g_fota_uri, sizeof(g_fota_uri));

    LOGI("Initiating Secure FOTA from Host: '%s', URI: '%s', Size: %u bytes",
         g_fota_host, g_fota_uri, g_expected_size);

    nwy_ssl_conf_t *fota_ssl_ctx = NULL;
    if (strncmp(url, "https", 5) == 0) {
        fota_ssl_ctx = nwy_hal_ssl_create_context(5, NULL, g_fota_host);
    }

    memset(&g_fota_http_param, 0, sizeof(nwy_http_param_t));
    g_fota_http_param.cid = 1;
    g_fota_http_param.host = g_fota_host;
    g_fota_http_param.port = (fota_ssl_ctx != NULL) ? 443 : 80;
    g_fota_http_param.timeout_s = 60;
    g_fota_http_param.cb = fota_http_result_cb;

    g_fota_http_handle = nwy_http_setup(&g_fota_http_param, fota_ssl_ctx);
    if (!g_fota_http_handle) {
        LOGE("Failed to create HTTP client for FOTA!");
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
    if (g_fota_http_handle != NULL) {
        nwy_http_close(g_fota_http_handle);
        g_fota_http_handle = NULL;
        nwy_hal_ssl_destroy_context(5);
        g_fota_state = FOTA_STATE_IDLE;
        LOGI("FOTA download cancelled by operator.");
    }
}
