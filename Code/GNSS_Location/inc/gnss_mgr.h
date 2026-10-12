/**
 * @file gnss_mgr.h
 * @brief Industrial Generic GNSS / Location Manager for Neoway N706B OpenCPU
 * @details Supports Native Satellite Engine (GPS, BeiDou, GLONASS, Galileo),
 *          NMEA filtering, parsed telemetry structures, battery-saving duty-cycle
 *          single-shot fixes, and persistent storage of last known valid fix
 *          into onboard P24C128D EEPROM with CRC-32 checksums.
 * 
 * Hardware Architecture:
 * - Module: Neoway N706B-CN-10 (ASR1605 platform, Cortex-R5 @ 614 MHz)
 * - Pin 2  : ANT_GNSS (50 Ohm RF track to JP3 Active/Passive Antenna Connector)
 * - Pin 44 : GNSS_BACKUP (Preserves ephemeris/RTC for sub-3s Hot Starts)
 * - Bias-T : VBAT -> R14 (10R) -> L2 (47nH) -> C21 (100pF) active antenna feed
 */

#ifndef __GNSS_MGR_H__
#define __GNSS_MGR_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nwy_loc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EEPROM Location Record Layout */
#define GNSS_EEPROM_MAGIC           0x474E5331  /* ASCII "GNS1" */
#define GNSS_EEPROM_STORAGE_ADDR    0x0300      /* EEPROM memory address for fix record */

/* Thread-safe formatted logging to debug UART */
void gnss_log(const char *fmt, ...);

/* Error Codes */
typedef enum {
    GNSS_OK                 =  0,
    GNSS_ERR_NOT_INIT       = -1,
    GNSS_ERR_BUSY           = -2,
    GNSS_ERR_TIMEOUT        = -3,
    GNSS_ERR_INVALID_PARAM  = -4,
    GNSS_ERR_HW_FAIL        = -5,
    GNSS_ERR_NO_FIX         = -6,
    GNSS_ERR_EEPROM         = -7
} gnss_status_t;

/* Supported Constellation Modes */
typedef enum {
    GNSS_MODE_GPS                   = 0,
    GNSS_MODE_BEIDOU                = 1,
    GNSS_MODE_GPS_BEIDOU            = 2,    /* Recommended for Asia/India */
    GNSS_MODE_GPS_GLONASS           = 3,    /* Recommended for Europe/Global */
    GNSS_MODE_GPS_GALILEO           = 4,
    GNSS_MODE_BEIDOU_GLONASS        = 5,
    GNSS_MODE_BEIDOU_GALILEO        = 6,
    GNSS_MODE_GLONASS_GALILEO       = 7,
    GNSS_MODE_GLONASS               = 8,
    GNSS_MODE_GALILEO               = 9,
    GNSS_MODE_GPS_BEIDOU_GLONASS    = 10,   /* Triple Constellation (Maximum satellite visibility) */
    GNSS_MODE_GPS_BEIDOU_GALILEO    = 11,
    GNSS_MODE_GPS_GLONASS_GALILEO   = 12,
    GNSS_MODE_ALL_FOUR              = 14    /* GPS + BeiDou + GLONASS + Galileo */
} gnss_constellation_mode_t;

/* GNSS Fix Quality Status */
typedef enum {
    GNSS_FIX_NONE       = 0,
    GNSS_FIX_2D         = 1,
    GNSS_FIX_3D         = 2,
    GNSS_FIX_DGPS       = 3
} gnss_fix_type_t;

/* Parsed GNSS Telemetry Structure */
typedef struct {
    bool            is_valid;           /* True if coordinates are locked and valid */
    gnss_fix_type_t fix_type;           /* None, 2D, or 3D fix */
    uint8_t         satellites_used;    /* Number of satellites used in fix */
    uint8_t         satellites_view;    /* Satellites in view */
    double          latitude;           /* Latitude in signed decimal degrees (+North, -South) */
    double          longitude;          /* Longitude in signed decimal degrees (+East, -West) */
    float           altitude_m;         /* Altitude in meters above mean sea level */
    float           speed_kmh;          /* Ground speed in km/h */
    float           course_deg;         /* Heading / Course over ground (0 - 359.9 deg) */
    float           hdop;               /* Horizontal Dilution of Precision (lower is better) */
    float           pdop;               /* Position Dilution of Precision */
    float           vdop;               /* Vertical Dilution of Precision */
    uint32_t        utc_time;           /* UTC time seconds */
    uint32_t        ttff_ms;            /* Time-To-First-Fix in milliseconds (energy benchmark) */
    uint32_t        timestamp_sys_ms;   /* System uptime timestamp when fix was recorded */
} gnss_fix_data_t;

/* Non-Volatile EEPROM Record Structure (Stored with CRC-32) */
typedef struct {
    uint32_t magic;                     /* GNSS_EEPROM_MAGIC */
    uint32_t timestamp_sys_ms;          /* System timestamp */
    uint32_t utc_time;                  /* UTC epoch/seconds */
    double   latitude;                  /* Stored latitude */
    double   longitude;                 /* Stored longitude */
    float    altitude_m;                /* Stored altitude */
    float    speed_kmh;                 /* Stored speed */
    float    course_deg;                /* Stored course */
    float    hdop;                      /* Stored HDOP */
    uint8_t  satellites_used;           /* Satellite count */
    uint8_t  fix_type;                  /* 2D / 3D */
    uint8_t  startup_mode;              /* Hot / Warm / Cold */
    uint8_t  reserved;
    uint32_t ttff_ms;                   /* Energy metric: Duration required to acquire */
    uint32_t crc32;                     /* IEEE 802.3 CRC-32 */
} __attribute__((packed)) gnss_nv_record_t;

/* High-Precision Filtered Fix Structure */
typedef struct {
    bool     is_converged;         /* True if at least 5 samples have been integrated */
    uint16_t sample_count;         /* Number of samples currently in the averaging window */
    double   filtered_lat;         /* High-precision HDOP-weighted latitude */
    double   filtered_lon;         /* High-precision HDOP-weighted longitude */
    float    filtered_alt;         /* Filtered altitude above sea level in meters */
    float    estimated_accuracy_m; /* Estimated horizontal error radius (+/- meters) */
    float    avg_hdop;             /* Mean HDOP across window */
} gnss_precision_fix_t;

/* Raw NMEA Stream Callback Definition */
typedef void (*gnss_nmea_raw_cb_t)(const char *nmea_sentence, uint16_t len);

/* Fix Acquired Event Callback Definition */
typedef void (*gnss_fix_event_cb_t)(const gnss_fix_data_t *fix);

/* Configuration Descriptor */
typedef struct {
    gnss_constellation_mode_t constellation;    /* Default constellation mode */
    nwy_loc_startup_mode_e    startup_mode;     /* Hot, Warm, Cold start */
    nwy_loc_rate_e            output_rate;      /* 1Hz, 2Hz, 5Hz, 10Hz */
    uint32_t                  nmea_mask;        /* Mask of enabled NMEA sentences */
    gnss_nmea_raw_cb_t        nmea_cb;          /* Optional raw callback */
    gnss_fix_event_cb_t       fix_cb;           /* Optional fix notification callback */
    bool                      enable_eeprom_sync;/* Automatically store valid fixes to EEPROM */
} gnss_config_t;

/* =========================================================================
 * CORE GNSS MANAGEMENT API
 * ========================================================================= */

/**
 * @brief Initialize the generic GNSS manager subsystem.
 * @param cfg Pointer to configuration struct (or NULL for industrial defaults).
 * @return GNSS_OK on success, negative error code on failure.
 */
gnss_status_t gnss_mgr_init(const gnss_config_t *cfg);

/**
 * @brief De-initialize the GNSS manager and release resources.
 */
void gnss_mgr_deinit(void);

/**
 * @brief Power ON and start satellite navigation engine.
 * @return GNSS_OK on success, negative on error.
 */
gnss_status_t gnss_mgr_power_on(void);

/**
 * @brief Power OFF satellite navigation engine to save battery power (0 mA draw).
 * @return GNSS_OK on success, negative on error.
 */
gnss_status_t gnss_mgr_power_off(void);

/**
 * @brief Check if the GNSS RF receiver hardware is currently powered and tracking.
 * @return true if running, false if powered down.
 */
bool gnss_mgr_is_powered(void);

/**
 * @brief Enable or disable direct live raw NMEA sentence streaming to debug UART.
 * @param enable true to stream all raw NMEA lines live, false to suppress.
 */
void gnss_mgr_set_raw_nmea_streaming(bool enable);

/**
 * @brief Set the satellite constellation mode.
 * @param mode Desired constellation (e.g., GNSS_MODE_GPS_BEIDOU_GLONASS).
 */
gnss_status_t gnss_mgr_set_constellation(gnss_constellation_mode_t mode);

/**
 * @brief Set the restart/startup behavior (Hot, Warm, Cold).
 * @param startup Startup mode.
 */
gnss_status_t gnss_mgr_set_startup_mode(nwy_loc_startup_mode_e startup);

/**
 * @brief Set the fix/output rate (1Hz, 2Hz, 5Hz, 10Hz).
 */
gnss_status_t gnss_mgr_set_rate(nwy_loc_rate_e rate);

/**
 * @brief Configure which NMEA sentences are generated.
 * @param nmea_mask Bitwise mask (NWY_LOC_NMEA_MASK_RMC | NWY_LOC_NMEA_MASK_GGA etc.)
 */
gnss_status_t gnss_mgr_set_nmea_mask(uint32_t nmea_mask);

/* =========================================================================
 * TELEMETRY & BATTERY-OPTIMIZED FIX API
 * ========================================================================= */

/**
 * @brief Poll the current cached fix status and telemetry.
 * @param fix Pointer to destination fix data structure.
 * @return GNSS_OK if a valid lock is present, GNSS_ERR_NO_FIX if searching.
 */
gnss_status_t gnss_mgr_get_fix(gnss_fix_data_t *fix);

/**
 * @brief Retrieve the high-precision HDOP-weighted averaged fix.
 *        Cancels Gaussian atmospheric and multipath jitter over time.
 * @param out_pfix Destination structure for high-precision coordinates.
 * @return GNSS_OK on success, GNSS_ERR_NO_FIX if no samples available.
 */
gnss_status_t gnss_mgr_get_precision_fix(gnss_precision_fix_t *out_pfix);

/**
 * @brief Reset the high-precision averaging filter buffer.
 */
void gnss_mgr_reset_precision_filter(void);

/**
 * @brief Battery Optimization Core: Single-Shot Fix & Automatic Power Down.
 *        1. Powers ON the GNSS engine.
 *        2. Polls until a valid satellite lock is achieved or timeout expires.
 *        3. Immediately powers OFF the GNSS engine to minimize battery drain.
 *        4. Automatically saves the fix to EEPROM (if enabled).
 * 
 * @param fix_out Destination structure for the acquired fix coordinates.
 * @param timeout_sec Maximum time to search for satellites (e.g., 30 - 60 sec).
 * @return GNSS_OK if fix was acquired within timeout, GNSS_ERR_TIMEOUT if not fixed.
 */
gnss_status_t gnss_mgr_acquire_single_fix(gnss_fix_data_t *fix_out, uint32_t timeout_sec);

/**
 * @brief Restore the last-known valid fix from the onboard P24C128D EEPROM.
 * @param fix_out Destination structure for restored coordinates.
 * @return GNSS_OK if valid record was found and CRC-32 passed, negative on error.
 */
gnss_status_t gnss_mgr_load_last_known_fix_from_eeprom(gnss_fix_data_t *fix_out);

/**
 * @brief Save the given fix coordinates to the onboard P24C128D EEPROM with CRC-32.
 * @param fix Fix data to commit to non-volatile storage.
 * @return GNSS_OK on success, negative on error.
 */
gnss_status_t gnss_mgr_save_fix_to_eeprom(const gnss_fix_data_t *fix);

/**
 * @brief Provide Assistance Data (A-GNSS) to accelerate Time-To-First-Fix.
 * @param server_info Pointer to AGPS server parameters.
 * @return GNSS_OK on success, negative on failure.
 */
gnss_status_t gnss_mgr_inject_agnss(nwy_setserver_para_t *server_info);

#ifdef __cplusplus
}
#endif

#endif /* __GNSS_MGR_H__ */
