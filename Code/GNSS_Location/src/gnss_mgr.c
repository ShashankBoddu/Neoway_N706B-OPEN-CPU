/**
 * @file gnss_mgr.c
 * @brief Industrial Generic GNSS / Location Manager Implementation
 * @details Neoway N706B OpenCPU (ASR1605 Crane Platform)
 */

#include "gnss_mgr.h"
#include "eeprom_24cxx.h"
#include "nwy_osi_api.h"
#include "nwy_uart_api.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define DEBUG_UART_PORT "URT1"

/* Module State */
static bool             s_is_initialized = false;
static bool             s_is_powered     = false;
static gnss_config_t    s_cfg;
static gnss_fix_data_t  s_cached_fix;
static nwy_osi_mutex_t  s_gnss_mutex     = NULL;
static int              s_log_uart_fd    = -1;
static bool             s_raw_nmea_stream_enabled = false;

/* High-Precision Multi-Sample Averaging Filter Window */
#define GNSS_PRECISION_WINDOW_SIZE 30
typedef struct {
    double lat;
    double lon;
    float  alt;
    float  hdop;
} gnss_sample_point_t;

static gnss_sample_point_t s_precision_samples[GNSS_PRECISION_WINDOW_SIZE];
static uint16_t            s_precision_head  = 0;
static uint16_t            s_precision_count = 0;

void gnss_mgr_set_raw_nmea_streaming(bool enable)
{
    s_raw_nmea_stream_enabled = enable;
}

/* Formatted Debug Logging */
void gnss_log(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    if (len > 0) {
        if (buf[len - 1] != '\n') {
            buf[len++] = '\r';
            buf[len++] = '\n';
            buf[len] = '\0';
        }
        if (s_log_uart_fd < 0) {
            s_log_uart_fd = nwy_uart_open(DEBUG_UART_PORT, 115200, FC_NONE);
        }
        if (s_log_uart_fd >= 0) {
            nwy_uart_write(s_log_uart_fd, (unsigned char *)buf, (unsigned int)len);
        }
    }
}

/* Mutex Locking Helpers */
static bool gnss_lock(uint32_t timeout_ms)
{
    if (s_gnss_mutex == NULL) return false;
    return (nwy_sdk_mutex_lock(s_gnss_mutex, (int)timeout_ms) == NWY_SUCCESS);
}

static void gnss_unlock(void)
{
    if (s_gnss_mutex != NULL) {
        nwy_sdk_mutex_unlock(s_gnss_mutex);
    }
}

/* Internal Callback for Neoway NMEA Engine */
static void on_nwy_loc_nmea_ind(nwy_gnss_output_info_ind_t *ind_msg)
{
    if (ind_msg == NULL || ind_msg->length == 0) return;

    /* 1. Forward raw sentence to debug UART if live streaming is active */
    if (s_raw_nmea_stream_enabled) {
        if (s_log_uart_fd < 0) {
            s_log_uart_fd = nwy_uart_open(DEBUG_UART_PORT, 115200, FC_NONE);
        }
        if (s_log_uart_fd >= 0) {
            nwy_uart_write(s_log_uart_fd, (unsigned char *)ind_msg->nmea_data, ind_msg->length);
        }
    }

    /* 2. Forward raw sentence to user callback if registered */
    if (s_cfg.nmea_cb != NULL) {
        s_cfg.nmea_cb((const char *)ind_msg->nmea_data, ind_msg->length);
    }

    /* 2. Parse telemetry structure */
    nwy_gnss_nmea_info_t loc_info;
    memset(&loc_info, 0, sizeof(loc_info));

    if (nwy_loc_nmea_parse(&loc_info) == NWY_SUCCESS) {
        if (gnss_lock(100)) {
            /* Update cached fix */
            if (loc_info.fix_flag > 0 && loc_info.satellite_num >= 3) {
                s_cached_fix.is_valid         = true;
                s_cached_fix.fix_type         = (loc_info.satellite_num >= 4) ? GNSS_FIX_3D : GNSS_FIX_2D;
                s_cached_fix.satellites_used  = loc_info.satellite_num;
                s_cached_fix.satellites_view  = loc_info.satellite_num;
                s_cached_fix.latitude         = (double)loc_info.latitude;
                s_cached_fix.longitude        = (double)loc_info.longitude;
                s_cached_fix.altitude_m       = loc_info.altitude;
                s_cached_fix.speed_kmh        = loc_info.speed;
                s_cached_fix.course_deg       = loc_info.course;
                s_cached_fix.hdop             = loc_info.m_nHDOP;
                s_cached_fix.pdop             = loc_info.m_nPDOP;
                s_cached_fix.vdop             = loc_info.m_nVDOP;
                s_cached_fix.utc_time         = loc_info.m_nUTCTime;
                s_cached_fix.timestamp_sys_ms = (uint32_t)nwy_uptime_get();

                /* Feed the High-Precision Filter if fix is 3D with healthy HDOP */
                if (loc_info.satellite_num >= 4 && loc_info.m_nHDOP > 0.05f && loc_info.m_nHDOP <= 2.5f) {
                    s_precision_samples[s_precision_head].lat  = (double)loc_info.latitude;
                    s_precision_samples[s_precision_head].lon  = (double)loc_info.longitude;
                    s_precision_samples[s_precision_head].alt  = loc_info.altitude;
                    s_precision_samples[s_precision_head].hdop = loc_info.m_nHDOP;
                    s_precision_head = (s_precision_head + 1) % GNSS_PRECISION_WINDOW_SIZE;
                    if (s_precision_count < GNSS_PRECISION_WINDOW_SIZE) {
                        s_precision_count++;
                    }
                }

                /* Notify user fix event callback */
                if (s_cfg.fix_cb != NULL) {
                    s_cfg.fix_cb(&s_cached_fix);
                }
            } else {
                /* Searching: update satellite count even if no valid fix yet */
                s_cached_fix.satellites_view = loc_info.satellite_num;
            }
            gnss_unlock();
        }
    }
}

/* =========================================================================
 * CORE API IMPLEMENTATION
 * ========================================================================= */

gnss_status_t gnss_mgr_init(const gnss_config_t *cfg)
{
    if (s_gnss_mutex == NULL) {
        nwy_sdk_mutex_create(&s_gnss_mutex);
    }

    if (!gnss_lock(1000)) return GNSS_ERR_BUSY;

    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        /* Industrial Defaults */
        s_cfg.constellation      = GNSS_MODE_GPS_BEIDOU;         /* Mode 2: GPS + BeiDou (Supported on N706B) */
        s_cfg.startup_mode       = NWY_LOC_HOT_START;            /* Fast hot start */
        s_cfg.output_rate        = NWY_LOC_1HZ;                  /* 1 Hz standard rate */
        s_cfg.nmea_mask          = NWY_LOC_NMEA_MASK_ALL;
        s_cfg.nmea_cb            = NULL;
        s_cfg.fix_cb             = NULL;
        s_cfg.enable_eeprom_sync = true;
    }

    memset(&s_cached_fix, 0, sizeof(s_cached_fix));
    s_is_powered = false;

    /* Register low-level NMEA reception callback */
    nwy_error_e err = nwy_loc_recv_cb_reg((nwy_loc_ind_event_func)on_nwy_loc_nmea_ind);
    if (err != NWY_SUCCESS) {
        gnss_log("  [GNSS ERROR] Failed to register NMEA callback (code: %d)", err);
        gnss_unlock();
        return GNSS_ERR_HW_FAIL;
    }

    /* Configure operating parameters */
    nwy_loc_mode_set((int)s_cfg.constellation);
    nwy_loc_startup_set(s_cfg.startup_mode);
    nwy_loc_rate_set(s_cfg.output_rate);
    nwy_loc_cfg_nmea_type(s_cfg.nmea_mask);

    s_is_initialized = true;
    gnss_unlock();

    gnss_log("  [GNSS INIT] Generic GNSS Engine initialized successfully (Constellation=%d, Rate=1Hz).",
             s_cfg.constellation);

    return GNSS_OK;
}

void gnss_mgr_deinit(void)
{
    if (!gnss_lock(1000)) return;

    if (s_is_powered) {
        nwy_loc_navigation_stop();
        s_is_powered = false;
    }

    nwy_loc_recv_cb_reg(NULL);
    s_is_initialized = false;

    gnss_unlock();
}

gnss_status_t gnss_mgr_power_on(void)
{
    if (!s_is_initialized) return GNSS_ERR_NOT_INIT;
    if (!gnss_lock(1000)) return GNSS_ERR_BUSY;

    if (s_is_powered) {
        gnss_unlock();
        return GNSS_OK;
    }

    nwy_error_e err = nwy_loc_navigation_start();
    if (err != NWY_SUCCESS) {
        gnss_log("  [GNSS ERROR] nwy_loc_navigation_start failed (code: %d)", err);
        gnss_unlock();
        return GNSS_ERR_HW_FAIL;
    }

    s_is_powered = true;
    gnss_unlock();

    gnss_log("  [GNSS POWER] Satellite receiver powered ON (RF frontend active, ~45mA).");
    return GNSS_OK;
}

gnss_status_t gnss_mgr_power_off(void)
{
    if (!s_is_initialized) return GNSS_ERR_NOT_INIT;
    if (!gnss_lock(1000)) return GNSS_ERR_BUSY;

    if (!s_is_powered) {
        gnss_unlock();
        return GNSS_OK;
    }

    nwy_error_e err = nwy_loc_navigation_stop();
    if (err != NWY_SUCCESS) {
        gnss_log("  [GNSS ERROR] nwy_loc_navigation_stop failed (code: %d)", err);
        gnss_unlock();
        return GNSS_ERR_HW_FAIL;
    }

    s_is_powered = false;
    gnss_unlock();

    gnss_log("  [GNSS POWER] Satellite receiver powered OFF (0 mA draw, battery preserved).");
    return GNSS_OK;
}

bool gnss_mgr_is_powered(void)
{
    return s_is_powered;
}

gnss_status_t gnss_mgr_set_constellation(gnss_constellation_mode_t mode)
{
    if (!gnss_lock(500)) return GNSS_ERR_BUSY;
    s_cfg.constellation = mode;
    nwy_error_e err = nwy_loc_mode_set((int)mode);
    gnss_unlock();
    return (err == NWY_SUCCESS) ? GNSS_OK : GNSS_ERR_HW_FAIL;
}

gnss_status_t gnss_mgr_set_startup_mode(nwy_loc_startup_mode_e startup)
{
    if (!gnss_lock(500)) return GNSS_ERR_BUSY;
    s_cfg.startup_mode = startup;
    nwy_error_e err = nwy_loc_startup_set(startup);
    gnss_unlock();
    return (err == NWY_SUCCESS) ? GNSS_OK : GNSS_ERR_HW_FAIL;
}

gnss_status_t gnss_mgr_set_rate(nwy_loc_rate_e rate)
{
    if (!gnss_lock(500)) return GNSS_ERR_BUSY;
    s_cfg.output_rate = rate;
    nwy_error_e err = nwy_loc_rate_set(rate);
    gnss_unlock();
    return (err == NWY_SUCCESS) ? GNSS_OK : GNSS_ERR_HW_FAIL;
}

gnss_status_t gnss_mgr_set_nmea_mask(uint32_t nmea_mask)
{
    if (!gnss_lock(500)) return GNSS_ERR_BUSY;
    s_cfg.nmea_mask = nmea_mask;
    nwy_error_e err = nwy_loc_cfg_nmea_type(nmea_mask);
    gnss_unlock();
    return (err == NWY_SUCCESS) ? GNSS_OK : GNSS_ERR_HW_FAIL;
}

gnss_status_t gnss_mgr_get_fix(gnss_fix_data_t *fix)
{
    if (fix == NULL) return GNSS_ERR_INVALID_PARAM;
    if (!s_is_initialized) return GNSS_ERR_NOT_INIT;

    if (!gnss_lock(200)) return GNSS_ERR_BUSY;
    *fix = s_cached_fix;
    bool valid = s_cached_fix.is_valid;
    gnss_unlock();

    return valid ? GNSS_OK : GNSS_ERR_NO_FIX;
}

gnss_status_t gnss_mgr_get_precision_fix(gnss_precision_fix_t *out_pfix)
{
    if (out_pfix == NULL) return GNSS_ERR_INVALID_PARAM;
    memset(out_pfix, 0, sizeof(*out_pfix));

    if (!gnss_lock(200)) return GNSS_ERR_BUSY;

    if (s_precision_count == 0) {
        gnss_unlock();
        return GNSS_ERR_NO_FIX;
    }

    double sum_weight   = 0.0;
    double weighted_lat = 0.0;
    double weighted_lon = 0.0;
    double weighted_alt = 0.0;
    float  sum_hdop     = 0.0f;

    for (uint16_t i = 0; i < s_precision_count; i++) {
        float h = s_precision_samples[i].hdop;
        if (h < 0.2f) h = 0.2f;
        double w = 1.0 / (double)(h * h);
        sum_weight   += w;
        weighted_lat += s_precision_samples[i].lat * w;
        weighted_lon += s_precision_samples[i].lon * w;
        weighted_alt += (double)s_precision_samples[i].alt * w;
        sum_hdop     += s_precision_samples[i].hdop;
    }

    out_pfix->sample_count = s_precision_count;
    out_pfix->is_converged = (s_precision_count >= 5);
    out_pfix->filtered_lat = weighted_lat / sum_weight;
    out_pfix->filtered_lon = weighted_lon / sum_weight;
    out_pfix->filtered_alt = (float)(weighted_alt / sum_weight);
    out_pfix->avg_hdop     = sum_hdop / (float)s_precision_count;

    /* Estimated horizontal accuracy radius ~ (avg_hdop * 2.5m) / sqrt(N) */
    float sigma_single = out_pfix->avg_hdop * 2.5f;
    if (sigma_single < 1.0f) sigma_single = 1.0f;
    float sqrt_n = 1.0f;
    if (s_precision_count >= 25)      sqrt_n = 5.0f;
    else if (s_precision_count >= 16) sqrt_n = 4.0f;
    else if (s_precision_count >= 9)  sqrt_n = 3.0f;
    else if (s_precision_count >= 4)  sqrt_n = 2.0f;
    out_pfix->estimated_accuracy_m = sigma_single / sqrt_n;

    gnss_unlock();
    return GNSS_OK;
}

void gnss_mgr_reset_precision_filter(void)
{
    if (gnss_lock(200)) {
        s_precision_head  = 0;
        s_precision_count = 0;
        memset(s_precision_samples, 0, sizeof(s_precision_samples));
        gnss_unlock();
    }
}

/* =========================================================================
 * BATTERY OPTIMIZATION CORE: SINGLE-SHOT DUTY FIX & POWER DOWN
 * ========================================================================= */

gnss_status_t gnss_mgr_acquire_single_fix(gnss_fix_data_t *fix_out, uint32_t timeout_sec)
{
    if (!s_is_initialized) return GNSS_ERR_NOT_INIT;
    if (fix_out == NULL) return GNSS_ERR_INVALID_PARAM;

    gnss_log("");
    gnss_log(">>> [GNSS BATTERY-OPTIMIZER] Starting single-shot fix cycle (Timeout: %u s)...", timeout_sec);

    /* 1. Clear previous fix cache */
    if (gnss_lock(500)) {
        s_cached_fix.is_valid = false;
        gnss_unlock();
    }

    /* 2. Power ON satellite engine */
    int64 t_start = nwy_uptime_get();
    gnss_status_t pwr_st = gnss_mgr_power_on();
    if (pwr_st != GNSS_OK) {
        return pwr_st;
    }

    /* 3. Non-blocking active polling loop */
    bool fixed = false;
    uint32_t elapsed_sec = 0;

    while (elapsed_sec < timeout_sec) {
        nwy_thread_sleep(1000); /* 1-second non-blocking RTOS sleep */
        elapsed_sec++;

        if (gnss_lock(200)) {
            if (s_cached_fix.is_valid) {
                fixed = true;
                *fix_out = s_cached_fix;
                fix_out->ttff_ms = (uint32_t)(nwy_uptime_get() - t_start);
            } else if (elapsed_sec % 5 == 0 || elapsed_sec == 1) {
                gnss_log("  [SEARCHING %02u/%02u s] Visible satellites: %u | HDOP: %.2f (Tracking sky signals...)",
                         elapsed_sec, timeout_sec, s_cached_fix.satellites_view, s_cached_fix.hdop);
            }
            gnss_unlock();
        }

        if (fixed) break;
    }

    /* 4. CRITICAL BATTERY SAVER: Power OFF receiver immediately */
    gnss_mgr_power_off();

    if (fixed) {
        gnss_log(">>> [GNSS FIX ACQUIRED] TTFF: %u ms (~%.1f s) | Satellites: %u | HDOP: %.2f",
                 fix_out->ttff_ms, fix_out->ttff_ms / 1000.0f, fix_out->satellites_used, fix_out->hdop);
        gnss_log("    Coordinates: Lat=%.6f, Lon=%.6f, Alt=%.1fm, Spd=%.1fkm/h",
                 fix_out->latitude, fix_out->longitude, fix_out->altitude_m, fix_out->speed_kmh);

        /* 5. Synchronize with non-volatile EEPROM */
        if (s_cfg.enable_eeprom_sync) {
            gnss_mgr_save_fix_to_eeprom(fix_out);
        }
        return GNSS_OK;
    } else {
        gnss_log(">>> [GNSS TIMEOUT] No satellite lock after %u s. Receiver shut down to conserve battery.",
                 timeout_sec);
        return GNSS_ERR_TIMEOUT;
    }
}

/* =========================================================================
 * NON-VOLATILE EEPROM STORAGE INTEGRATION
 * ========================================================================= */

gnss_status_t gnss_mgr_save_fix_to_eeprom(const gnss_fix_data_t *fix)
{
    if (fix == NULL) return GNSS_ERR_INVALID_PARAM;

    gnss_nv_record_t rec;
    memset(&rec, 0, sizeof(rec));

    rec.magic            = GNSS_EEPROM_MAGIC;
    rec.timestamp_sys_ms = fix->timestamp_sys_ms;
    rec.utc_time         = fix->utc_time;
    rec.latitude         = fix->latitude;
    rec.longitude        = fix->longitude;
    rec.altitude_m       = fix->altitude_m;
    rec.speed_kmh        = fix->speed_kmh;
    rec.course_deg       = fix->course_deg;
    rec.hdop             = fix->hdop;
    rec.satellites_used  = fix->satellites_used;
    rec.fix_type         = (uint8_t)fix->fix_type;
    rec.startup_mode     = (uint8_t)s_cfg.startup_mode;
    rec.ttff_ms          = fix->ttff_ms;

    /* Calculate CRC-32 over payload */
    size_t payload_len = offsetof(gnss_nv_record_t, crc32);
    rec.crc32 = eeprom_crc32((const uint8_t *)&rec, payload_len);

    /* Write record to EEPROM */
    int ret = eeprom_write(GNSS_EEPROM_STORAGE_ADDR, (const uint8_t *)&rec, sizeof(rec));
    if (ret == 0) {
        gnss_log("  [EEPROM] Last-Known Position stored at 0x%04X (CRC-32: 0x%08X).",
                 GNSS_EEPROM_STORAGE_ADDR, rec.crc32);
        return GNSS_OK;
    } else {
        gnss_log("  [EEPROM ERROR] Failed to save GNSS record to EEPROM (code: %d)", ret);
        return GNSS_ERR_EEPROM;
    }
}

gnss_status_t gnss_mgr_load_last_known_fix_from_eeprom(gnss_fix_data_t *fix_out)
{
    if (fix_out == NULL) return GNSS_ERR_INVALID_PARAM;

    gnss_nv_record_t rec;
    memset(&rec, 0, sizeof(rec));

    int ret = eeprom_read(GNSS_EEPROM_STORAGE_ADDR, (uint8_t *)&rec, sizeof(rec));
    if (ret != 0) {
        gnss_log("  [EEPROM NOTICE] Unable to read EEPROM at address 0x%04X (code: %d)",
                 GNSS_EEPROM_STORAGE_ADDR, ret);
        return GNSS_ERR_EEPROM;
    }

    if (rec.magic != GNSS_EEPROM_MAGIC) {
        gnss_log("  [EEPROM NOTICE] No valid GNSS record found at 0x%04X (magic: 0x%08X).",
                 GNSS_EEPROM_STORAGE_ADDR, rec.magic);
        return GNSS_ERR_NO_FIX;
    }

    /* Verify CRC-32 */
    size_t payload_len = offsetof(gnss_nv_record_t, crc32);
    uint32_t calc_crc = eeprom_crc32((const uint8_t *)&rec, payload_len);
    if (calc_crc != rec.crc32) {
        gnss_log("  [EEPROM ERROR] Corrupted GNSS record (CRC got: 0x%08X, expected: 0x%08X)!",
                 calc_crc, rec.crc32);
        return GNSS_ERR_EEPROM;
    }

    /* Unpack into fix_out */
    fix_out->is_valid         = true;
    fix_out->fix_type         = (gnss_fix_type_t)rec.fix_type;
    fix_out->satellites_used  = rec.satellites_used;
    fix_out->satellites_view  = rec.satellites_used;
    fix_out->latitude         = rec.latitude;
    fix_out->longitude        = rec.longitude;
    fix_out->altitude_m       = rec.altitude_m;
    fix_out->speed_kmh        = rec.speed_kmh;
    fix_out->course_deg       = rec.course_deg;
    fix_out->hdop             = rec.hdop;
    fix_out->utc_time         = rec.utc_time;
    fix_out->ttff_ms          = rec.ttff_ms;
    fix_out->timestamp_sys_ms = rec.timestamp_sys_ms;

    gnss_log("  [EEPROM RESTORE] Last-Known Position restored from EEPROM (Lat=%.6f, Lon=%.6f, Alt=%.1fm, HDOP=%.2f).",
             fix_out->latitude, fix_out->longitude, fix_out->altitude_m, fix_out->hdop);

    return GNSS_OK;
}

gnss_status_t gnss_mgr_inject_agnss(nwy_setserver_para_t *server_info)
{
    if (server_info == NULL) return GNSS_ERR_INVALID_PARAM;
    nwy_error_e err = nwy_loc_agnss_start(server_info);
    return (err == NWY_SUCCESS) ? GNSS_OK : GNSS_ERR_HW_FAIL;
}
