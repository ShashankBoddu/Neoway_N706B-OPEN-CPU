/**
 * @file data_collector.c
 * @brief BME688 TinyML Data Collector & Two-Way SMS Command Engine Implementation
 */

#include "data_collector.h"
#include "remote_sd_manager.h"
#include "mqtt_manager.h"
#include "hal/nwy_hal_secure_fota.h"
#include "i2c_bus.h"
#include "nwy_sd_card.h"
#include "ff.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/pm/nwy_hal_pm.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define LOG_TAG "[DATA_COLLECTOR] "

static bme68x_dev_t       *s_bme_dev = NULL;
static collector_status_t  s_status;
static FIL                 s_csv_file;
static FIL                 s_bme_file;
static bool                s_csv_file_is_open = false;
static bool                s_bme_file_is_open = false;
static bool                s_bme_first_entry = true;
static nwy_osi_mutex_t     s_collector_mutex = NULL;

static char                s_alert_phone[32] = DEFAULT_ALERT_PHONE;
static bool                s_boot_alert_sent = false;
static bool                s_alert_sensor_error_sent = false;
static bool                s_alert_sd_error_sent = false;
static bool                s_alert_sd_low_sent = false;

/* Official Bosch HP-354 step starting offsets from cycle start (ms)
 * Step 0: 320°C (700ms)  -> +0ms
 * Step 1: 100°C (280ms)  -> +700ms
 * Step 2: 100°C (1400ms) -> +980ms
 * Step 3: 100°C (4032ms) -> +2380ms
 * Step 4: 200°C (700ms)  -> +6412ms
 * Step 5: 200°C (700ms)  -> +7112ms
 * Step 6: 200°C (700ms)  -> +7812ms
 * Step 7: 320°C (700ms)  -> +8512ms
 * Step 8: 320°C (700ms)  -> +9212ms
 * Step 9: 320°C (700ms)  -> +9912ms */
static const uint32_t s_hp354_step_offsets_ms[10] = {
    0, 700, 980, 2380, 6412, 7112, 7812, 8512, 9212, 9912
};

static uint32_t s_session_rtc_base = 0;
static uint32_t s_session_start_uptime_ms = 0;

static uint32_t rtc_to_epoch(const nwy_time_t *tm, int tz_quarter_hours)
{
    int year = (tm->year < 100) ? (2000 + tm->year) : tm->year;
    if (year < 1970) return 1731834000;

    static const uint16_t days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    uint32_t days = (uint32_t)((year - 1970) * 365 + ((year - 1969) / 4));
    int m = tm->mon - 1;
    if (m >= 0 && m < 12) {
        days += days_before_month[m];
        if (m > 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
            days++;
        }
    }
    days += (tm->day > 0) ? (uint32_t)(tm->day - 1) : 0;
    uint32_t local_sec = days * 86400 + (uint32_t)tm->hour * 3600 + (uint32_t)tm->min * 60 + (uint32_t)tm->sec;

    /* Subtract cellular network timezone offset (15-min intervals) to get true UTC Unix epoch */
    int32_t tz_offset_sec = (int32_t)tz_quarter_hours * 900;
    return (uint32_t)((int64_t)local_sec - tz_offset_sec);
}

/* Canonical Bosch BME AI-Studio v3.2.1 Configuration Header & Body */
static const char s_bme_config_header_body[] =
    "{\"configHeader\":{\"dateCreated_ISO\":\"2026-11-17T09:00:00Z\",\"appVersion\":\"3.2.1\",\"boardType\":\"board_8\",\"boardMode\":\"burn_in\",\"boardLayout\":\"grouped\"},"
    "\"configBody\":{\"heaterProfiles\":[{\"id\":\"heater_354\",\"timeBase\":140,\"temperatureTimeVectors\":[[320,5],[100,2],[100,10],[100,30],[200,5],[200,5],[200,5],[320,5],[320,5],[320,5]]}],"
    "\"dutyCycleProfiles\":[{\"id\":\"duty_1\",\"numberScanningCycles\":1,\"numberSleepingCycles\":0}],"
    "\"sensorConfigurations\":["
    "{\"sensorIndex\":0,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":1,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":2,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":3,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":4,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":5,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":6,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"},"
    "{\"sensorIndex\":7,\"heaterProfile\":\"heater_354\",\"dutyCycleProfile\":\"duty_1\"}]},\n";

/* Canonical Bosch BME AI-Studio 13-Column Data Definition */
static const char s_bme_datacolumns[] =
    "\"rawDataBody\":{\"dataColumns\":["
    "{\"name\":\"Sensor Index\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"sensor_index\",\"colId\":1},"
    "{\"name\":\"Sensor ID\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"sensor_id\",\"colId\":2},"
    "{\"name\":\"Time Since PowerOn\",\"unit\":\"Milliseconds\",\"format\":\"integer\",\"key\":\"timestamp_since_poweron\",\"colId\":3},"
    "{\"name\":\"Real time clock\",\"unit\":\"Unix Timestamp: seconds since Jan 01 1970. (UTC); 0 = missing\",\"format\":\"integer\",\"key\":\"real_time_clock\",\"colId\":4},"
    "{\"name\":\"Temperature\",\"unit\":\"DegreesCelcius\",\"format\":\"float\",\"key\":\"temperature\",\"colId\":5},"
    "{\"name\":\"Pressure\",\"unit\":\"Hectopascals\",\"format\":\"float\",\"key\":\"pressure\",\"colId\":6},"
    "{\"name\":\"Relative Humidity\",\"unit\":\"Percent\",\"format\":\"float\",\"key\":\"relative_humidity\",\"colId\":7},"
    "{\"name\":\"Resistance Gassensor\",\"unit\":\"Ohms\",\"format\":\"float\",\"key\":\"resistance_gassensor\",\"colId\":8},"
    "{\"name\":\"Heater Profile Step Index\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"heater_profile_step_index\",\"colId\":9},"
    "{\"name\":\"Scanning Mode Enabled\",\"unit\":\"\",\"format\":\"boolean\",\"key\":\"scanning_enabled\",\"colId\":10},"
    "{\"name\":\"Scanning Cycle Index\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"scanning_cycle_index\",\"colId\":11},"
    "{\"name\":\"Label Tag\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"label_tag\",\"colId\":12},"
    "{\"name\":\"Error Code\",\"unit\":\"\",\"format\":\"integer\",\"key\":\"error_code\",\"colId\":13}"
    "],\"dataBlock\":[";

/* -------------------------------------------------------------------------
 * HELPER STRING UTILITIES
 * ------------------------------------------------------------------------- */
static void trim_whitespace(char *str)
{
    if (!str) return;

    /* Trim leading whitespace */
    char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    /* Trim trailing whitespace */
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[len - 1] = '\0';
        len--;
    }
}

static void sanitize_label(char *dest, const char *src, size_t max_len)
{
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d < (max_len - 1); s++) {
        char c = src[s];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            dest[d++] = (char)toupper((unsigned char)c);
        } else if (isspace((unsigned char)c)) {
            dest[d++] = '_';
        }
    }
    dest[d] = '\0';
    if (d == 0) {
        strncpy(dest, "UNSPECIFIED", max_len - 1);
        dest[max_len - 1] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * PROACTIVE ALERT SMS DISPATCHER (LATCHED - SENT ONCE PER EVENT)
 * ------------------------------------------------------------------------- */
void data_collector_set_alert_phone(const char *phone)
{
    if (!phone) return;
    strncpy(s_alert_phone, phone, sizeof(s_alert_phone) - 1);
    s_alert_phone[sizeof(s_alert_phone) - 1] = '\0';
    trim_whitespace(s_alert_phone);
    nwy_hal_uart_printf(1, LOG_TAG "Alert recipient phone updated to: %s\r\n", s_alert_phone);
}

const char *data_collector_get_alert_phone(void)
{
    return s_alert_phone;
}

void data_collector_send_alert(const char *alert_msg)
{
    if (!alert_msg || alert_msg[0] == '\0') return;

    if (s_alert_phone[0] != '\0') {
        nwy_hal_uart_printf(1, LOG_TAG "DISPATCHING ALERT SMS to %s: \"%s\"\r\n", s_alert_phone, alert_msg);
        bool sent = nwy_hal_sms_send(1, s_alert_phone, alert_msg);
        if (!sent) {
            nwy_hal_uart_printf(1, LOG_TAG "WARNING: Alert SMS send failed to %s!\r\n", s_alert_phone);
        }
    }

    /* Mirror alert notification to MQTT telemetry stream */
    mqtt_manager_publish(alert_msg);
}

void data_collector_send_boot_status(bool bme_ok, bool sd_ok,const char *ver)
{
    if (s_boot_alert_sent) return; /* Enforce strictly once on startup */
    s_boot_alert_sent = true;

    char msg[200];
    uint64_t free_bytes = nwy_sdcard_get_free_space();
    float free_gb = (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);

    if (bme_ok && sd_ok) {
        snprintf(msg, sizeof(msg),
                 "[BME688 READY] Online!\n Ver: %s\n Sensor:OK(0x76), SD:OK(%.1fGB), State:IDLE. Send 'START <LABEL>' (e.g. START BADFOOD)",ver,
                 free_gb);
    } else {
        snprintf(msg, sizeof(msg),
                 "[BME688 ALERT] BOOT: %s%s Check hardware wiring!",
                 !bme_ok ? "Sensor Missing (0x76)! " : "",
                 !sd_ok ? "SD Card Mount Failed! " : "");
    }

    data_collector_send_alert(msg);
}

/* -------------------------------------------------------------------------
 * INITIALIZATION
 * ------------------------------------------------------------------------- */
bool data_collector_is_sensor_ready(void)
{
    if (s_bme_dev == NULL) return false;
    if (s_bme_dev->chip_id != BME68X_CHIP_ID) return false;
    return bme68x_check_connection(s_bme_dev);
}

int data_collector_check_sensor_health(void)
{
    if (s_bme_dev == NULL) return -1;

    if (bme68x_check_connection(s_bme_dev)) {
        s_status.sensor_interfaced = true;
        return 0;
    }

    nwy_hal_uart_printf(1, LOG_TAG "Sensor unresponsive! Performing bus reset & auto-recovery...\r\n");
    s_status.sensor_interfaced = false;

    /* Reset I2C bus lines and clear any stuck slave state */
    i2c_bus_reset();
    nwy_hal_os_thread_sleep(50);

    /* Attempt driver re-initialization */
    int8_t rslt = bme68x_init(s_bme_dev);
    if (rslt == BME68X_OK && s_bme_dev->chip_id == BME68X_CHIP_ID) {
        s_status.sensor_interfaced = true;
        nwy_hal_uart_printf(1, LOG_TAG "Sensor recovery SUCCESSFUL! Chip ID: 0x%02X\r\n", s_bme_dev->chip_id);
        return 0;
    }

    nwy_hal_uart_printf(1, LOG_TAG "Sensor recovery FAILED (err: %d, id: 0x%02X)\r\n", rslt, s_bme_dev->chip_id);
    return -2;
}

int data_collector_init(bme68x_dev_t *bme_dev)
{
    s_bme_dev = bme_dev;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = COLLECTOR_STATE_IDLE;
    s_status.interval_sec = 12; /* Default 12 seconds (~10.8s HP-354 scan + 1.2s rest) */
    strncpy(s_status.current_label, "CLEAN_AIR", sizeof(s_status.current_label) - 1);
    s_status.sensor_interfaced = (bme_dev != NULL && bme_dev->chip_id == BME68X_CHIP_ID);
    s_status.sd_mounted = nwy_sdcard_is_mounted();

    if (s_collector_mutex == NULL) {
        nwy_sdk_mutex_create(&s_collector_mutex);
    }

    nwy_hal_uart_printf(1, LOG_TAG "Engine initialized. Default interval: %d s | Sensor: %s\r\n",
                        s_status.interval_sec, s_status.sensor_interfaced ? "READY" : "OFFLINE");
    return 0;
}

/* -------------------------------------------------------------------------
 * START RECORDING SESSION
 * ------------------------------------------------------------------------- */
int data_collector_start(const char *label, const char *sender_phone, char *reply_buf, size_t reply_max)
{
    if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 2000);

    if (s_status.state == COLLECTOR_STATE_RECORDING) {
        if (reply_buf && reply_max > 0) {
            snprintf(reply_buf, reply_max,
                     "ALREADY RUNNING: Session '%s' active (%lu samples). Send STOP before starting new.",
                     s_status.current_label, (unsigned long)s_status.sample_count);
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return -1;
    }

    /* Strict Guard 1: Verify SD Card is mounted */
    if (!nwy_sdcard_is_mounted()) {
        if (nwy_sdcard_reinit() != 0) {
            if (reply_buf && reply_max > 0) {
                snprintf(reply_buf, reply_max, "ERROR: MicroSD card missing or unmounted! Cannot start recording.");
            }
            if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
            return -2;
        }
    }

    /* Strict Guard 2: Verify BME688 sensor is interfaced successfully */
    if (!data_collector_is_sensor_ready()) {
        /* Run one auto-recovery attempt in case of transient bus glitch */
        if (data_collector_check_sensor_health() != 0) {
            if (reply_buf && reply_max > 0) {
                snprintf(reply_buf, reply_max,
                         "REJECTED: BME688 sensor not interfaced (0x76)! Fix wiring before collecting '%s'.",
                         label ? label : "DATA");
            }
            if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
            return -4;
        }
    }

    /* Sanitize label */
    sanitize_label(s_status.current_label, label, sizeof(s_status.current_label));
    if (sender_phone) {
        strncpy(s_status.last_sender_phone, sender_phone, sizeof(s_status.last_sender_phone) - 1);
    }

    /* Find next available file index */
    char file_path[64];
    int file_idx = 1;
    for (; file_idx < 1000; file_idx++) {
        snprintf(file_path, sizeof(file_path), "0:/dataset_%s_%03d.csv", s_status.current_label, file_idx);
        if (!nwy_sdcard_exists(file_path)) {
            break;
        }
    }

    strncpy(s_status.current_file_path, file_path, sizeof(s_status.current_file_path) - 1);

    /* Open new file for writing */
    if (!nwy_sdcard_lock(5000)) {
        if (reply_buf && reply_max > 0) {
            snprintf(reply_buf, reply_max, "ERROR: MicroSD card busy, lock timeout.");
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return -3;
    }

    FRESULT res = f_open(&s_csv_file, file_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        nwy_sdcard_unlock();
        if (reply_buf && reply_max > 0) {
            snprintf(reply_buf, reply_max, "ERROR: Failed to create file '%s' (FatFs err: %d)", file_path, res);
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return -3;
    }

    s_csv_file_is_open = true;

    /* Write CSV Header */
    const char *header = "timestamp_ms,sample_idx,label,temp_c,humidity_rh,pressure_hpa,"
                         "r_gas_0,r_gas_1,r_gas_2,r_gas_3,r_gas_4,r_gas_5,r_gas_6,r_gas_7,r_gas_8,r_gas_9\r\n";
    UINT bw = 0;
    f_write(&s_csv_file, header, (UINT)strlen(header), &bw);
    f_sync(&s_csv_file);

    /* Construct Bosch BME AI-Studio native (.bmerawdata) filename & JSON header */
    nwy_time_t tm;
    int tz = 0;
    bool time_valid = false;
    char iso_time[48] = "2026-11-16T12:00:00Z";
    char iso_prefix[32] = "2026-11-16T12:00:00";
    char bme_filename[128];
    uint32_t session_seed = (uint32_t)(nwy_uptime_get() & 0xFFFF);

    s_session_start_uptime_ms = (uint32_t)nwy_uptime_get();

    if (nwy_hal_pm_get_time(&tm, &tz)) {
        int full_year = (tm.year < 100) ? (2000 + tm.year) : tm.year;
        if (full_year >= 2024 && tm.mon >= 1 && tm.mon <= 12 && tm.day >= 1 && tm.day <= 31) {
            time_valid = true;
            s_session_rtc_base = rtc_to_epoch(&tm, tz);
            int tz_h = tz / 4;
            int tz_m = (tz >= 0 ? (tz % 4) : (-tz % 4)) * 15;
            snprintf(iso_time, sizeof(iso_time), "%04d-%02d-%02dT%02d:%02d:%02d%+03d:%02d",
                     full_year, tm.mon, tm.day, tm.hour, tm.min, tm.sec, tz_h, tz_m);
            snprintf(iso_prefix, sizeof(iso_prefix), "%04d-%02d-%02dT%02d:%02d:%02d",
                     full_year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
            snprintf(bme_filename, sizeof(bme_filename),
                     "0:/%04d%02d%02d_%02d%02d_%s_0.bmerawdata",
                     full_year, tm.mon, tm.day, tm.hour, tm.min, s_status.current_label);
        }
    }
    if (!time_valid) {
        uint32_t up_s = (uint32_t)(nwy_uptime_get() / 1000);
        s_session_rtc_base = 1731834000 + up_s;
        snprintf(iso_time, sizeof(iso_time), "2026-11-17T%02u:%02u:%02uZ",
                 (unsigned int)((up_s / 3600) % 24),
                 (unsigned int)((up_s / 60) % 60),
                 (unsigned int)(up_s % 60));
        snprintf(iso_prefix, sizeof(iso_prefix), "2026-11-17T%02u:%02u:%02u",
                 (unsigned int)((up_s / 3600) % 24),
                 (unsigned int)((up_s / 60) % 60),
                 (unsigned int)(up_s % 60));
        snprintf(bme_filename, sizeof(bme_filename),
                 "0:/20261117_%02u%02u_%s_0.bmerawdata",
                 (unsigned int)((up_s / 3600) % 24),
                 (unsigned int)((up_s / 60) % 60),
                 s_status.current_label);
    }
    strncpy(s_status.current_bme_path, bme_filename, sizeof(s_status.current_bme_path) - 1);

    FRESULT bme_res = f_open(&s_bme_file, bme_filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (bme_res == FR_OK) {
        s_bme_file_is_open = true;
        s_bme_first_entry = true;

        UINT bw_bme = 0;
        /* Chunk 1: Static configuration header & body */
        f_write(&s_bme_file, s_bme_config_header_body, (UINT)strlen(s_bme_config_header_body), &bw_bme);

        /* Chunk 2: Dynamic rawDataHeader */
        char raw_hdr[512];
        snprintf(raw_hdr, sizeof(raw_hdr),
            "  \"rawDataHeader\": {\n"
            "    \"counterPowerOnOff\": 1,\n"
            "    \"seedPowerOnOff\": \"%04X\",\n"
            "    \"counterFileLimit\": 0,\n"
            "    \"dateCreated\": \"%u\",\n"
            "    \"dateCreated_ISO\": \"%s\",\n"
            "    \"firmwareVersion\": \"2.1.0\",\n"
            "    \"boardId\": \"Board_N706B\"\n"
            "  },\n",
            (unsigned int)session_seed, (unsigned int)s_session_rtc_base, iso_time);
        f_write(&s_bme_file, raw_hdr, (UINT)strlen(raw_hdr), &bw_bme);

        /* Chunk 3: Static canonical dataColumns & opening dataBlock bracket */
        f_write(&s_bme_file, s_bme_datacolumns, (UINT)strlen(s_bme_datacolumns), &bw_bme);
        f_sync(&s_bme_file);
        nwy_hal_uart_printf(1, LOG_TAG "Bosch AI-Studio (.bmerawdata) initialized: %s\r\n", bme_filename);
    } else {
        nwy_hal_uart_printf(1, LOG_TAG "WARNING: Could not create .bmerawdata file (err %d)\r\n", bme_res);
    }
    nwy_sdcard_unlock();

    s_status.sample_count = 0;
    s_status.state = COLLECTOR_STATE_RECORDING;
    s_alert_sensor_error_sent = false;
    s_alert_sd_error_sent = false;
    s_alert_sd_low_sent = false;

    nwy_hal_uart_printf(1, LOG_TAG "RECORDING STARTED: %s | File: %s\r\n", s_status.current_label, file_path);

    if (reply_buf && reply_max > 0) {
        /* Extract file basename for compact SMS */
        const char *basename = file_path;
        if (strncmp(basename, "0:/", 3) == 0) basename += 3;
        snprintf(reply_buf, reply_max,
                 "STARTED: '%s' (%s, %lus)",
                 s_status.current_label, basename, (unsigned long)s_status.interval_sec);
    }

    if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
    return 0;
}

/* -------------------------------------------------------------------------
 * STOP RECORDING SESSION
 * ------------------------------------------------------------------------- */
int data_collector_stop(char *reply_buf, size_t reply_max)
{
    if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 2000);

    if (s_status.state != COLLECTOR_STATE_RECORDING) {
        if (reply_buf && reply_max > 0) {
            snprintf(reply_buf, reply_max, "IDLE: No active session. Send START <LABEL>");
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return 0;
    }

    if (!nwy_sdcard_lock(5000)) {
        nwy_hal_uart_printf(1, LOG_TAG "ERROR: SD lock timeout during stop\r\n");
    } else {
        if (s_bme_file_is_open) {
            const char *footer = "\n    ]\n  }\n}\n";
            UINT bw_bme = 0;
            f_write(&s_bme_file, footer, (UINT)strlen(footer), &bw_bme);
            f_sync(&s_bme_file);
            f_close(&s_bme_file);
            s_bme_file_is_open = false;

            /* Generate companion .bmelabelinfo file so Bosch AI-Studio automatically identifies the label */
            char labelinfo_path[128];
            strncpy(labelinfo_path, s_status.current_bme_path, sizeof(labelinfo_path) - 1);
            labelinfo_path[sizeof(labelinfo_path) - 1] = '\0';
            char *dot = strstr(labelinfo_path, ".bmerawdata");
            if (dot) {
                strcpy(dot, ".bmelabelinfo");
                FIL linfo_file;
                if (f_open(&linfo_file, labelinfo_path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                    char linfo_buf[256];
                    int llen = snprintf(linfo_buf, sizeof(linfo_buf),
                        "{\n"
                        "  \"labelInformation\": [\n"
                        "    {\n"
                        "      \"labelTag\": 1,\n"
                        "      \"labelName\": \"%s\",\n"
                        "      \"labelDescription\": \"Recorded on Neoway N706B\"\n"
                        "    }\n"
                        "  ]\n"
                        "}\n", s_status.current_label);
                    UINT bw_li = 0;
                    if (llen > 0) {
                        f_write(&linfo_file, linfo_buf, (UINT)llen, &bw_li);
                    }
                    f_close(&linfo_file);
                    nwy_hal_uart_printf(1, LOG_TAG "Companion label info created: %s\r\n", labelinfo_path);
                }
            }
        }

        if (s_csv_file_is_open) {
            f_sync(&s_csv_file);
            f_close(&s_csv_file);
            s_csv_file_is_open = false;
        }

        nwy_sdcard_unlock();
    }

    s_status.state = COLLECTOR_STATE_IDLE;

    const char *csv_base = s_status.current_file_path;
    if (strncmp(csv_base, "0:/", 3) == 0) csv_base += 3;

    nwy_hal_uart_printf(1, LOG_TAG "RECORDING STOPPED: %s | Total samples: %lu\r\n",
                        s_status.current_label, (unsigned long)s_status.sample_count);

    if (reply_buf && reply_max > 0) {
        snprintf(reply_buf, reply_max,
                 "STOPPED: '%s' (%lu samples). File: %s",
                 s_status.current_label, (unsigned long)s_status.sample_count, csv_base);
    }

    if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
    return 0;
}

/* -------------------------------------------------------------------------
 * QUERY STATUS & LIVE SENSOR TELEMETRY
 * ------------------------------------------------------------------------- */
void data_collector_get_status(collector_status_t *status_out, char *reply_buf, size_t reply_max)
{
    if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 2000);

    bool bme_ok = data_collector_is_sensor_ready();
    bool sd_ok = nwy_sdcard_is_mounted();
    s_status.sensor_interfaced = bme_ok;
    s_status.sd_mounted = sd_ok;

    if (status_out) {
        *status_out = s_status;
    }

    uint64_t free_bytes = nwy_sdcard_get_free_space();
    float free_gb = (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);

    if (reply_buf && reply_max > 0) {
        if (!bme_ok) {
            snprintf(reply_buf, reply_max,
                     "STATUS: SENSOR ERR(0x76)! State:%s | SD:%s(%.1fGB)",
                     (s_status.state == COLLECTOR_STATE_RECORDING) ? "PAUSED" : "IDLE",
                     sd_ok ? "OK" : "ERR", free_gb);
        } else if (s_status.state == COLLECTOR_STATE_RECORDING) {
            snprintf(reply_buf, reply_max,
                     "STATUS: RUN '%s'(%lu) | T:%.1fC H:%.1f%% | SD:%.1fGB",
                     s_status.current_label, (unsigned long)s_status.sample_count,
                     s_status.last_temperature, s_status.last_humidity, free_gb);
        } else {
            snprintf(reply_buf, reply_max,
                     "STATUS: IDLE | T:%.1fC H:%.1f%% R0:%.1fk | SD:%.1fGB",
                     s_status.last_temperature, s_status.last_humidity,
                     s_status.last_rgas_0 / 1000.0f, free_gb);
        }
    }

    if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
}

/* -------------------------------------------------------------------------
 * SET SAMPLING INTERVAL
 * ------------------------------------------------------------------------- */
int data_collector_set_interval(uint32_t interval_sec, char *reply_buf, size_t reply_max)
{
    if (interval_sec < 11) interval_sec = 11;       /* 10 HP-354 steps = ~10.8s minimum scan time */
    if (interval_sec > 300) interval_sec = 300;   /* Max 5 minutes */

    if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 2000);
    s_status.interval_sec = interval_sec;

    nwy_hal_uart_printf(1, LOG_TAG "Sampling interval updated: %lu seconds\r\n", (unsigned long)interval_sec);

    if (reply_buf && reply_max > 0) {
        snprintf(reply_buf, reply_max, "INTERVAL: Set to %lu s.", (unsigned long)interval_sec);
    }

    if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
    return 0;
}

static bool extract_json_str(const char *json, const char *key, char *out, int max_len) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    char *end = strchr(start, '\"');
    if (!end || (end - start) >= max_len) return false;
    strncpy(out, start, end - start);
    out[end - start] = '\0';
    return true;
}

static bool extract_json_int(const char *json, const char *key, uint32_t *out) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    while (*start == ' ') start++;
    *out = (uint32_t)atoi(start);
    return true;
}

/* -------------------------------------------------------------------------
 * UNIFIED COMMAND PARSER & DISPATCHER (SMS, MQTT, CLI)
 * ------------------------------------------------------------------------- */
void data_collector_execute_command(const char *cmd_text, const char *sender_id, char *reply, size_t reply_max)
{
    if (!cmd_text) return;
    if (reply && reply_max > 0) memset(reply, 0, reply_max);

    char cmd[384];
    strncpy(cmd, cmd_text, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    trim_whitespace(cmd);

    /* Strip telecom spam warning prefix injected by cellular operator (e.g. "Airtel Warning: SPAM | ") */
    char *pipe_pos = strchr(cmd, '|');
    if (pipe_pos && (strstr(cmd, "SPAM") || strstr(cmd, "spam") || strstr(cmd, "Warning") || strstr(cmd, "Airtel"))) {
        memmove(cmd, pipe_pos + 1, strlen(pipe_pos + 1) + 1);
        trim_whitespace(cmd);
    }

    nwy_hal_uart_printf(1, "CMD [%s]: %s\r\n", sender_id ? sender_id : "?", cmd);

    /* Convert command prefix to uppercase for case-insensitive matching */
    char cmd_upper[384];
    size_t i = 0;
    for (; cmd[i] != '\0' && i < sizeof(cmd_upper) - 1; i++) {
        cmd_upper[i] = (char)toupper((unsigned char)cmd[i]);
    }
    cmd_upper[i] = '\0';

    if (strncmp(cmd_upper, "START", 5) == 0 ||
        strncmp(cmd_upper, "RECORD", 6) == 0 ||
        strncmp(cmd_upper, "COLLECT", 7) == 0 ||
        strncmp(cmd_upper, "DATA", 4) == 0) {
        
        const char *label_part = cmd + 5;
        if (strncmp(cmd_upper, "RECORD", 6) == 0) label_part = cmd + 6;
        else if (strncmp(cmd_upper, "COLLECT", 7) == 0) label_part = cmd + 7;
        else if (strncmp(cmd_upper, "DATA", 4) == 0) label_part = cmd + 4;

        while (*label_part && isspace((unsigned char)*label_part)) {
            label_part++;
        }

        if (*label_part == '\0') {
            label_part = "BASELINE_AIR";
        }

        data_collector_start(label_part, sender_id, reply, reply_max);
    }
    else if (strcmp(cmd_upper, "STOP") == 0) {
        data_collector_stop(reply, reply_max);
    }
    else if (strncmp(cmd_upper, "STATUS", 6) == 0 || strncmp(cmd_upper, "STATE", 5) == 0) {
        const char *extra = cmd + 6;
        if (strncmp(cmd_upper, "STATE", 5) == 0) extra = cmd + 5;
        while (*extra && isspace((unsigned char)*extra)) extra++;

        if (*extra != '\0') {
            /* User asked: e.g. "STATUS BADFOOD" or "STATUS TOILET_CLEAN" */
            bool bme_live = data_collector_is_sensor_ready();
            bool sd_live = nwy_sdcard_is_mounted();
            uint64_t free_b = nwy_sdcard_get_free_space();
            float free_g = (float)free_b / (1024.0f * 1024.0f * 1024.0f);

            if (!bme_live) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max,
                             "STATUS: NOT READY for '%s'! Sensor not interfaced (0x76). Check wiring.", extra);
            } else if (!sd_live) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max,
                             "STATUS: NOT READY for '%s'! MicroSD missing/unmounted. Insert card.", extra);
            } else if (s_status.state == COLLECTOR_STATE_RECORDING) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max,
                             "STATUS: BUSY '%s' (%lu). Send STOP then START %s.",
                             s_status.current_label, (unsigned long)s_status.sample_count, extra);
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max,
                             "STATUS: READY for '%s'. Sensor: OK(%.1fC), SD: %.1fGB.",
                             extra, s_status.last_temperature, free_g);
            }
        } else {
            data_collector_get_status(NULL, reply, reply_max);
        }
    }
    else if (strncmp(cmd_upper, "INTERVAL", 8) == 0) {
        const char *sec_str = cmd + 8;
        while (*sec_str && isspace((unsigned char)*sec_str)) sec_str++;
        uint32_t interval = (uint32_t)atoi(sec_str);
        data_collector_set_interval(interval, reply, reply_max);
    }
    else if (strncmp(cmd_upper, "ALERT", 5) == 0) {
        const char *phone_arg = cmd + 5;
        while (*phone_arg && isspace((unsigned char)*phone_arg)) phone_arg++;
        if (*phone_arg != '\0') {
            data_collector_set_alert_phone(phone_arg);
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "Alerts: %s", s_alert_phone);
        } else {
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "Alerts: %s. Send 'ALERT <PH>'", s_alert_phone);
        }
    }
    else if (strncmp(cmd_upper, "LIST", 4) == 0 ||
             strncmp(cmd_upper, "IIST", 4) == 0 ||
             strncmp(cmd_upper, "1IST", 4) == 0 ||
             strncmp(cmd_upper, "SD LIST", 7) == 0) {
        remote_sd_list_files(reply, reply_max);
    }
    else if (strncmp(cmd_upper, "UPLOAD", 6) == 0) {
        const char *target = cmd + 6;
        while (*target && isspace((unsigned char)*target)) target++;

        char target_upper[64] = {0};
        for (size_t ti = 0; target[ti] && ti < sizeof(target_upper) - 1; ti++) {
            target_upper[ti] = (char)toupper((unsigned char)target[ti]);
        }

        sd_upload_request_t req;
        memset(&req, 0, sizeof(req));
        if (sender_id && strcmp(sender_id, "MQTT") != 0) {
            strncpy(req.notify_phone, sender_id, sizeof(req.notify_phone) - 1);
        } else {
            strncpy(req.notify_phone, s_alert_phone, sizeof(req.notify_phone) - 1);
        }

        if (strcmp(target_upper, "LATEST") == 0 || strcmp(target_upper, "LAST") == 0) {
            req.target_type = UPLOAD_TARGET_LATEST;
            if (remote_sd_queue_upload(&req)) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD: Queued latest to %s",
                             remote_sd_get_server_url());
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD ERR: Busy");
            }
        } else if (strcmp(target_upper, "ALL") == 0) {
            req.target_type = UPLOAD_TARGET_ALL;
            if (remote_sd_queue_upload(&req)) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD: Queued ALL to %s",
                             remote_sd_get_server_url());
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD ERR: Busy");
            }
        } else if (strlen(target) > 0) {
            req.target_type = UPLOAD_TARGET_SINGLE;
            strncpy(req.filename, target, sizeof(req.filename) - 1);
            if (remote_sd_queue_upload(&req)) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD: Queued '%s' to %s",
                             target, remote_sd_get_server_url());
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "UPLOAD ERR: Busy");
            }
        } else {
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "Usage: UPLOAD LATEST|ALL|<FILE>");
        }
    }
    else if (strncmp(cmd_upper, "SERVER", 6) == 0 || strstr(cmd_upper, "SERVER")) {
        const char *s_pos = strstr(cmd_upper, "SERVER");
        const char *url_arg = cmd + (s_pos - cmd_upper) + 6;
        while (*url_arg && (isspace((unsigned char)*url_arg) || *url_arg == ':' || *url_arg == '=')) url_arg++;

        /* Handle user typos like "http: http://" or extra prefixes */
        const char *http_p = strstr(url_arg, "http://");
        if (!http_p) http_p = strstr(url_arg, "https://");
        if (!http_p) http_p = strstr(url_arg, "tcp://");
        if (http_p) {
            url_arg = http_p;
        }

        /* Validate URL format: must not contain spaces or shell keywords */
        if (*url_arg == '\0') {
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "SERVER: %s. Send 'SERVER <URL>'",
                         remote_sd_get_server_url());
        } else if (strchr(url_arg, ' ') != NULL || strstr(url_arg, "ssh") != NULL ||
                   (strncmp(url_arg, "http://", 7) != 0 && strncmp(url_arg, "https://", 8) != 0 && strncmp(url_arg, "tcp://", 6) != 0)) {
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "SERVER ERR: Expected 'SERVER http://host:port/upload'");
        } else {
            remote_sd_set_server_url(url_arg);
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "SERVER: URL set to %s", remote_sd_get_server_url());
        }
    }
    else if (strstr(cmd, "\"command\":\"OTA\"") || strstr(cmd, "\"command\": \"OTA\"") || strstr(cmd, "\"command\":\"ota\"")) {
        char fota_url[192] = {0};
        char fota_sig[160] = {0};
        uint32_t fota_size = 0;

        bool has_url = extract_json_str(cmd, "OTAConfigURL", fota_url, sizeof(fota_url));
        bool has_sig = extract_json_str(cmd, "signature", fota_sig, sizeof(fota_sig));
        bool has_size = extract_json_int(cmd, "Configsize", &fota_size);

        if (has_url && has_sig && has_size) {
            nwy_hal_uart_printf(1, LOG_TAG "Valid JSON OTA Trigger! URL: %s, Size: %lu\r\n", fota_url, (unsigned long)fota_size);
            const char *notify_dst = (sender_id && strcmp(sender_id, "MQTT") != 0) ? sender_id : s_alert_phone;
            bool started = nwy_hal_secure_fota_start_with_notify(fota_url, fota_size, fota_sig, notify_dst);
            if (started) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "FOTA: Download %s (%lu B)...", fota_url, (unsigned long)fota_size);
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "FOTA ERR: Failed to start or busy.");
            }
        } else {
            if (reply && reply_max > 0)
                snprintf(reply, reply_max, "FOTA ERR: Malformed trigger");
        }
    }
    else if (strncmp(cmd_upper, "FOTA", 4) == 0 || strncmp(cmd_upper, "OTA", 3) == 0) {
        const char *args = cmd + (strncmp(cmd_upper, "FOTA", 4) == 0 ? 4 : 3);
        while (*args && isspace((unsigned char)*args)) args++;

        char fota_url[192] = {0};
        uint32_t fota_size = 0;
        char fota_sig[160] = {0};

        if (*args == '\0' || strcmp(args, "UPDATE") == 0 || strcmp(args, "NOW") == 0 || strcmp(args, "LATEST") == 0) {
            /* Mode 1: Bare "FOTA" command - auto-derive URL from configured server */
            const char *srv_url = remote_sd_get_server_url();
            if (srv_url && strlen(srv_url) > 7) {
                strncpy(fota_url, srv_url, sizeof(fota_url) - 1);
                char *upload_slash = strstr(fota_url, "/upload");
                if (upload_slash) {
                    strcpy(upload_slash, "/nwy_open_app_fota.pkt");
                } else {
                    size_t flen = strlen(fota_url);
                    if (fota_url[flen - 1] == '/') fota_url[flen - 1] = '\0';
                    strncat(fota_url, "/nwy_open_app_fota.pkt", sizeof(fota_url) - strlen(fota_url) - 1);
                }
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "FOTA ERR: No server URL configured.");
            }
        } else if (isdigit((unsigned char)*args)) {
            /* Mode 2: FOTA <size> <sig> using configured server URL */
            const char *srv_url = remote_sd_get_server_url();
            if (srv_url && strlen(srv_url) > 7) {
                strncpy(fota_url, srv_url, sizeof(fota_url) - 1);
                char *upload_slash = strstr(fota_url, "/upload");
                if (upload_slash) {
                    strcpy(upload_slash, "/nwy_open_app_fota.pkt");
                } else {
                    size_t flen = strlen(fota_url);
                    if (fota_url[flen - 1] == '/') fota_url[flen - 1] = '\0';
                    strncat(fota_url, "/nwy_open_app_fota.pkt", sizeof(fota_url) - strlen(fota_url) - 1);
                }
                unsigned long fota_size_raw = 0;
                int parsed = sscanf(args, "%lu %159s", &fota_size_raw, fota_sig);
                if (parsed >= 1 && fota_size_raw > 0) {
                    fota_size = (uint32_t)fota_size_raw;
                }
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "FOTA ERR: No server URL configured.");
            }
        } else {
            /* Mode 3: FOTA <url> <size> <sig> OR FOTA <url> */
            unsigned long fota_size_raw = 0;
            int parsed = sscanf(args, "%191s %lu %159s", fota_url, &fota_size_raw, fota_sig);
            if (parsed == 3 && fota_size_raw > 0 && strlen(fota_sig) > 10) {
                fota_size = (uint32_t)fota_size_raw;
            } else {
                sscanf(args, "%191s", fota_url);
                fota_size = 0;
                fota_sig[0] = '\0';
            }
        }

        if (fota_url[0] != '\0') {
            nwy_hal_uart_printf(1, LOG_TAG "Starting FOTA from: %s (size: %lu, sig: %s)\r\n",
                                fota_url, (unsigned long)fota_size, (fota_sig[0] != '\0') ? "MANUAL" : "AUTO");
            const char *notify_dst = (sender_id && strcmp(sender_id, "MQTT") != 0) ? sender_id : s_alert_phone;
            bool started = nwy_hal_secure_fota_start_with_notify(fota_url, fota_size,
                                                                 (fota_sig[0] != '\0') ? fota_sig : NULL,
                                                                 notify_dst);
            if (started) {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max,
                             "FOTA: Download %s...",
                             fota_url);
            } else {
                if (reply && reply_max > 0)
                    snprintf(reply, reply_max, "FOTA ERR: Failed to start or busy.");
            }
        }
    }
    else if (strcmp(cmd_upper, "HELP") == 0 || strcmp(cmd_upper, "?") == 0) {
        if (reply && reply_max > 0)
            snprintf(reply, reply_max, "CMD: START, STOP, STATUS, LIST, UPLOAD, SERVER, FOTA, INTERVAL");
    }
    else {
        if (reply && reply_max > 0)
            snprintf(reply, reply_max,
                     "Unknown cmd '%s'. Send HELP.", cmd);
    }
}

/* -------------------------------------------------------------------------
 * SMS COMMAND LISTENER & DISPATCHER (Dual-Channel: SMS + MQTT)
 * ------------------------------------------------------------------------- */
void data_collector_process_sms(const char *sender_phone, const char *msg_text)
{
    if (!sender_phone || !msg_text) return;

    nwy_hal_uart_printf(1, LOG_TAG "Processing SMS from %s: \"%s\"\r\n", sender_phone, msg_text);

    char reply[256];
    memset(reply, 0, sizeof(reply));

    data_collector_execute_command(msg_text, sender_phone, reply, sizeof(reply));

    /* Transmit two-way SMS reply back to sender */
    if (reply[0] != '\0') {
        nwy_hal_uart_printf(1, LOG_TAG "Sending SMS reply to %s: \"%s\"\r\n", sender_phone, reply);
        bool sent = nwy_hal_sms_send(1, sender_phone, reply);
        if (!sent) {
            nwy_hal_uart_printf(1, LOG_TAG "WARNING: Failed to send SMS reply!\r\n");
        }

        /* Dual-Channel: Mirror reply to MQTT publish topic */
        mqtt_manager_publish(reply);
    }
}

/* -------------------------------------------------------------------------
 * WORKER THREAD: PERIODIC 10-STEP MULTI-HEATER SAMPLING
 * ------------------------------------------------------------------------- */
void data_collector_worker_task(void *param)
{
    (void)param;
    float r_gas[10];
    float temp = 0.0f;
    float hum = 0.0f;
    float pres = 0.0f;
    char  csv_row[256];

    nwy_hal_uart_printf(1, LOG_TAG "Collector worker thread started.\r\n");

    while (1) {
        if (s_status.state == COLLECTOR_STATE_RECORDING && s_bme_dev != NULL) {
            /* 1. Runtime continuous sensor health verification */
            if (!data_collector_is_sensor_ready()) {
                nwy_hal_uart_printf(1, LOG_TAG "Sensor dropped offline at runtime! Pausing recording...\r\n");
                s_status.sensor_interfaced = false;

                if (!s_alert_sensor_error_sent) {
                    s_alert_sensor_error_sent = true;
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 ALERT] Sensor lost on '%s'!",
                             s_status.current_label);
                    data_collector_send_alert(alert);
                }

                /* Attempt auto-recovery */
                if (data_collector_check_sensor_health() == 0) {
                    s_alert_sensor_error_sent = false;
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 INFO] Sensor restored for '%s'.",
                             s_status.current_label);
                    data_collector_send_alert(alert);
                } else {
                    nwy_hal_os_thread_sleep(3000);
                    continue; /* Do not proceed to scan or write corrupted data */
                }
            }

            uint32_t scan_start_time = (uint32_t)nwy_uptime_get();

            /* 2. Execute 10-step HP-354 Multi-Heater Scanning (takes ~10.8 seconds) */
            int8_t rslt = bme68x_perform_10step_scan(s_bme_dev, r_gas, &temp, &hum, &pres);

            if (rslt == BME68X_OK) {
                if (s_alert_sensor_error_sent) {
                    s_alert_sensor_error_sent = false; /* Sensor recovered */
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 INFO] Sensor OK for '%s'.",
                             s_status.current_label);
                    data_collector_send_alert(alert);
                }

                if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 1000);

                s_status.sensor_interfaced = true;
                s_status.sample_count++;
                s_status.last_temperature = temp;
                s_status.last_humidity = hum;
                s_status.last_pressure = pres;
                s_status.last_rgas_0 = r_gas[0];

                /* Format CSV Data Row:
                 * timestamp_ms,sample_idx,label,temp_c,humidity_rh,pressure_hpa,R0..R9 */
                snprintf(csv_row, sizeof(csv_row),
                         "%u,%lu,%s,%.2f,%.2f,%.2f,"
                         "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\r\n",
                         (unsigned int)scan_start_time,
                         (unsigned long)s_status.sample_count,
                         s_status.current_label,
                         temp, hum, pres,
                         r_gas[0], r_gas[1], r_gas[2], r_gas[3], r_gas[4],
                         r_gas[5], r_gas[6], r_gas[7], r_gas[8], r_gas[9]);

                if (nwy_sdcard_lock(2000)) {
                    if (s_csv_file_is_open) {
                        UINT bw = 0;
                        FRESULT wr_res = f_write(&s_csv_file, csv_row, (UINT)strlen(csv_row), &bw);
                        FRESULT sync_res = f_sync(&s_csv_file); /* Guarantees immediate persistence to NAND flash */
                        if (wr_res != FR_OK || sync_res != FR_OK || bw == 0) {
                            if (!s_alert_sd_error_sent) {
                                s_alert_sd_error_sent = true;
                                char alert[160];
                                snprintf(alert, sizeof(alert),
                                         "[BME688 ALERT] SD write failed (%d/%d)!",
                                         wr_res, sync_res);
                                data_collector_send_alert(alert);
                            }
                        }
                    }

                    if (s_bme_file_is_open) {
                        char bme_entry[220];
                        UINT bw_bme = 0;

                        for (int step = 0; step < 10; step++) {
                            uint32_t step_time_ms = scan_start_time + s_hp354_step_offsets_ms[step];
                            uint32_t elapsed_ms = (step_time_ms >= s_session_start_uptime_ms) ? (step_time_ms - s_session_start_uptime_ms) : 0;
                            uint32_t step_rtc = s_session_rtc_base + (elapsed_ms / 1000);
                            /* Canonical 13-Column Bosch format:
                             * [sensor_index, sensor_id, timestamp_since_poweron, real_time_clock,
                             *  temperature, pressure(hPa), relative_humidity, resistance_gassensor,
                             *  heater_profile_step_index, scanning_enabled, scanning_cycle_index, label_tag, error_code] */
                            int elen = snprintf(bme_entry, sizeof(bme_entry),
                                "%s\n      [0, 1, %u, %u, %.2f, %.2f, %.2f, %.1f, %d, 1, %lu, 1, 0]",
                                s_bme_first_entry ? "" : ",",
                                (unsigned int)step_time_ms,
                                (unsigned int)step_rtc,
                                temp,
                                pres, /* pressure in Hectopascals (hPa) */
                                hum,
                                r_gas[step],
                                (int)step,
                                (unsigned long)s_status.sample_count);
                            s_bme_first_entry = false;
                            if (elen > 0) {
                                f_write(&s_bme_file, bme_entry, (UINT)elen, &bw_bme);
                            }
                        }
                        f_sync(&s_bme_file);
                    }
                    nwy_sdcard_unlock();
                }

                /* Check SD card space every 20 samples */
                if ((s_status.sample_count % 20) == 0) {
                    uint64_t free_sp = nwy_sdcard_get_free_space();
                    if (free_sp < (50ULL * 1024 * 1024) && free_sp > 0) {
                        if (!s_alert_sd_low_sent) {
                            s_alert_sd_low_sent = true;
                            char alert[160];
                            snprintf(alert, sizeof(alert),
                                     "[BME688 ALERT] MicroSD nearly full (<50MB). Recording stopped!");
                            data_collector_send_alert(alert);
                            data_collector_stop(NULL, 0);
                        }
                    }
                }

                if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);

                /* Output live progress to UART console */
                nwy_hal_uart_printf(1, "[DATA LOG #%lu] %s | T:%.1f C, H:%.1f %%, P:%.0f hPa | R0:%.0f..R9:%.0f Ohm\r\n",
                                    (unsigned long)s_status.sample_count, s_status.current_label,
                                    temp, hum, pres, r_gas[0], r_gas[9]);
            } else {
                nwy_hal_uart_printf(1, LOG_TAG "BME688 scan error code: %d at runtime\r\n", rslt);
                s_status.sensor_interfaced = false;
                if (!s_alert_sensor_error_sent) {
                    s_alert_sensor_error_sent = true;
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 ALERT] I2C error (%d) on '%s'!",
                             rslt, s_status.current_label);
                    data_collector_send_alert(alert);
                }
                /* Attempt auto-recovery */
                data_collector_check_sensor_health();
            }

            /* Calculate sleep duration to maintain configured sampling rate */
            uint32_t scan_end_time = (uint32_t)nwy_uptime_get();
            uint32_t elapsed_ms = scan_end_time - scan_start_time;
            uint32_t target_interval_ms = s_status.interval_sec * 1000;

            if (target_interval_ms > elapsed_ms) {
                nwy_hal_os_thread_sleep(target_interval_ms - elapsed_ms);
            } else {
                nwy_hal_os_thread_sleep(100);
            }
        } else {
            /* Idle loop when not recording */
            nwy_hal_os_thread_sleep(500);
        }
    }
}
