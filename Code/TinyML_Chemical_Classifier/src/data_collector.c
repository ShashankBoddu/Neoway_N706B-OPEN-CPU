/**
 * @file data_collector.c
 * @brief BME688 TinyML Data Collector & Two-Way SMS Command Engine Implementation
 */

#include "data_collector.h"
#include "nwy_sd_card.h"
#include "ff.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/os/nwy_hal_os.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define LOG_TAG "[DATA_COLLECTOR] "

static bme68x_dev_t       *s_bme_dev = NULL;
static collector_status_t  s_status;
static FIL                 s_csv_file;
static bool                s_file_is_open = false;
static nwy_osi_mutex_t     s_collector_mutex = NULL;

static char                s_alert_phone[32] = DEFAULT_ALERT_PHONE;
static bool                s_boot_alert_sent = false;
static bool                s_alert_sensor_error_sent = false;
static bool                s_alert_sd_error_sent = false;
static bool                s_alert_sd_low_sent = false;

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
    if (s_alert_phone[0] == '\0') return;

    nwy_hal_uart_printf(1, LOG_TAG "DISPATCHING ALERT SMS to %s: \"%s\"\r\n", s_alert_phone, alert_msg);
    bool sent = nwy_hal_sms_send(1, s_alert_phone, alert_msg);
    if (!sent) {
        nwy_hal_uart_printf(1, LOG_TAG "WARNING: Alert SMS send failed to %s!\r\n", s_alert_phone);
    }
}

void data_collector_send_boot_status(bool bme_ok, bool sd_ok)
{
    if (s_boot_alert_sent) return; /* Enforce strictly once on startup */
    s_boot_alert_sent = true;

    char msg[160];
    uint64_t free_bytes = nwy_sdcard_get_free_space();
    float free_gb = (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);

    if (bme_ok && sd_ok) {
        snprintf(msg, sizeof(msg),
                 "[BME688 READY] Online! Sensor:OK(0x76), SD:OK(%.1fGB), State:IDLE. Send 'START <LABEL>' (e.g. START BADFOOD)",
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
int data_collector_init(bme68x_dev_t *bme_dev)
{
    s_bme_dev = bme_dev;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = COLLECTOR_STATE_IDLE;
    s_status.interval_sec = 3; /* Default 3 seconds between 10-step scans */
    strncpy(s_status.current_label, "CLEAN_AIR", sizeof(s_status.current_label) - 1);

    if (s_collector_mutex == NULL) {
        nwy_sdk_mutex_create(&s_collector_mutex);
    }

    nwy_hal_uart_printf(1, LOG_TAG "Engine initialized. Default interval: %d s\r\n", s_status.interval_sec);
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

    /* Verify SD Card is mounted */
    if (!nwy_sdcard_is_mounted()) {
        if (nwy_sdcard_reinit() != 0) {
            if (reply_buf && reply_max > 0) {
                snprintf(reply_buf, reply_max, "ERROR: MicroSD card missing or unmounted! Cannot start recording.");
            }
            if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
            return -2;
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
    FRESULT res = f_open(&s_csv_file, file_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        if (reply_buf && reply_max > 0) {
            snprintf(reply_buf, reply_max, "ERROR: Failed to create file '%s' (FatFs err: %d)", file_path, res);
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return -3;
    }

    s_file_is_open = true;

    /* Write CSV Header */
    const char *header = "timestamp_ms,sample_idx,label,temp_c,humidity_rh,pressure_hpa,"
                         "r_gas_0,r_gas_1,r_gas_2,r_gas_3,r_gas_4,r_gas_5,r_gas_6,r_gas_7,r_gas_8,r_gas_9\r\n";
    UINT bw = 0;
    f_write(&s_csv_file, header, (UINT)strlen(header), &bw);
    f_sync(&s_csv_file);

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
                 "STARTED: Dataset '%s'. File: %s. Sampling every %lus.",
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
            snprintf(reply_buf, reply_max, "IDLE: No active collection session to stop. Send 'START <LABEL>'.");
        }
        if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
        return 0;
    }

    if (s_file_is_open) {
        f_sync(&s_csv_file);
        f_close(&s_csv_file);
        s_file_is_open = false;
    }

    s_status.state = COLLECTOR_STATE_IDLE;

    const char *basename = s_status.current_file_path;
    if (strncmp(basename, "0:/", 3) == 0) basename += 3;

    nwy_hal_uart_printf(1, LOG_TAG "RECORDING STOPPED: %s | Total samples: %lu\r\n",
                        s_status.current_label, (unsigned long)s_status.sample_count);

    if (reply_buf && reply_max > 0) {
        snprintf(reply_buf, reply_max,
                 "STOPPED: Session '%s' finished! Total samples: %lu. File saved: %s.",
                 s_status.current_label, (unsigned long)s_status.sample_count, basename);
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

    if (status_out) {
        *status_out = s_status;
    }

    uint64_t free_bytes = nwy_sdcard_get_free_space();
    float free_gb = (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);

    if (reply_buf && reply_max > 0) {
        if (s_status.state == COLLECTOR_STATE_RECORDING) {
            snprintf(reply_buf, reply_max,
                     "STATUS: RUNNING | Label: %s | Count: %lu | T: %.1fC | H: %.1f%% | P: %.0fhPa | R0: %.1fk | SD: %.1fGB free",
                     s_status.current_label, (unsigned long)s_status.sample_count,
                     s_status.last_temperature, s_status.last_humidity, s_status.last_pressure,
                     s_status.last_rgas_0 / 1000.0f, free_gb);
        } else {
            snprintf(reply_buf, reply_max,
                     "STATUS: IDLE | Last: %s (%lu samples) | T: %.1fC | H: %.1f%% | R0: %.1fk | SD: %.1fGB free",
                     s_status.current_label, (unsigned long)s_status.sample_count,
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
    if (interval_sec < 2) interval_sec = 2;       /* 10 steps * 140ms = 1.4s minimum scan time */
    if (interval_sec > 300) interval_sec = 300;   /* Max 5 minutes */

    if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 2000);
    s_status.interval_sec = interval_sec;

    nwy_hal_uart_printf(1, LOG_TAG "Sampling interval updated: %lu seconds\r\n", (unsigned long)interval_sec);

    if (reply_buf && reply_max > 0) {
        snprintf(reply_buf, reply_max, "INTERVAL: Sampling rate set to %lu seconds.", (unsigned long)interval_sec);
    }

    if (s_collector_mutex) nwy_sdk_mutex_unlock(s_collector_mutex);
    return 0;
}

/* -------------------------------------------------------------------------
 * SMS COMMAND PARSER & DISPATCHER
 * ------------------------------------------------------------------------- */
void data_collector_process_sms(const char *sender_phone, const char *msg_text)
{
    if (!sender_phone || !msg_text) return;

    char cmd[128];
    strncpy(cmd, msg_text, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    trim_whitespace(cmd);

    nwy_hal_uart_printf(1, LOG_TAG "Processing SMS from %s: \"%s\"\r\n", sender_phone, cmd);

    char reply[160];
    memset(reply, 0, sizeof(reply));

    /* Convert command prefix to uppercase for case-insensitive matching */
    char cmd_upper[128];
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

        data_collector_start(label_part, sender_phone, reply, sizeof(reply));
    }
    else if (strcmp(cmd_upper, "STOP") == 0) {
        data_collector_stop(reply, sizeof(reply));
    }
    else if (strncmp(cmd_upper, "STATUS", 6) == 0 || strncmp(cmd_upper, "STATE", 5) == 0) {
        const char *extra = cmd + 6;
        if (strncmp(cmd_upper, "STATE", 5) == 0) extra = cmd + 5;
        while (*extra && isspace((unsigned char)*extra)) extra++;

        if (*extra != '\0') {
            /* User asked: e.g. "STATUS BADFOOD" or "STATUS TOILET_CLEAN" */
            if (s_status.state == COLLECTOR_STATE_RECORDING) {
                snprintf(reply, sizeof(reply),
                         "STATUS: RUNNING '%s' (%lu samples). To collect '%s', send STOP then START %s.",
                         s_status.current_label, (unsigned long)s_status.sample_count, extra, extra);
            } else {
                snprintf(reply, sizeof(reply),
                         "STATUS: IDLE. Ready! To collect dataset for '%s', send: START %s",
                         extra, extra);
            }
        } else {
            data_collector_get_status(NULL, reply, sizeof(reply));
        }
    }
    else if (strncmp(cmd_upper, "INTERVAL", 8) == 0) {
        const char *sec_str = cmd + 8;
        while (*sec_str && isspace((unsigned char)*sec_str)) sec_str++;
        uint32_t interval = (uint32_t)atoi(sec_str);
        data_collector_set_interval(interval, reply, sizeof(reply));
    }
    else if (strncmp(cmd_upper, "ALERT", 5) == 0) {
        const char *phone_arg = cmd + 5;
        while (*phone_arg && isspace((unsigned char)*phone_arg)) phone_arg++;
        if (*phone_arg != '\0') {
            data_collector_set_alert_phone(phone_arg);
            snprintf(reply, sizeof(reply), "Alerts recipient updated to: %s", s_alert_phone);
        } else {
            snprintf(reply, sizeof(reply), "Current alerts recipient: %s. Send 'ALERT <PHONE>' to change.", s_alert_phone);
        }
    }
    else if (strcmp(cmd_upper, "HELP") == 0 || strcmp(cmd_upper, "?") == 0) {
        snprintf(reply, sizeof(reply),
                 "Commands:\nSTART <LABEL> (e.g. START BADFOOD, TOILET_CLEAN)\nSTOP\nSTATUS\nINTERVAL <SEC>\nALERT <PHONE>\nHELP");
    }
    else {
        /* User sent a raw text label without 'START', e.g. "BADFOOD" or "TOILET_DIRTY" */
        if (strlen(cmd) >= 2) {
            data_collector_start(cmd, sender_phone, reply, sizeof(reply));
        } else {
            snprintf(reply, sizeof(reply),
                     "Unknown command '%s'. Send 'START <LABEL>' (e.g. START BADFOOD) or 'HELP'.", cmd);
        }
    }

    /* Transmit two-way SMS reply back to sender */
    if (reply[0] != '\0') {
        nwy_hal_uart_printf(1, LOG_TAG "Sending SMS reply to %s: \"%s\"\r\n", sender_phone, reply);
        bool sent = nwy_hal_sms_send(1, sender_phone, reply);
        if (!sent) {
            nwy_hal_uart_printf(1, LOG_TAG "WARNING: Failed to send SMS reply!\r\n");
        }
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
            uint32_t scan_start_time = (uint32_t)nwy_uptime_get();

            /* Execute 10-step HP-354 Multi-Heater Scanning (takes ~1.65 seconds) */
            int8_t rslt = bme68x_perform_10step_scan(s_bme_dev, r_gas, &temp, &hum, &pres);

            if (rslt == BME68X_OK) {
                if (s_alert_sensor_error_sent) {
                    s_alert_sensor_error_sent = false; /* Sensor recovered */
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 INFO] Sensor recovered! Recording resumed for '%s'.",
                             s_status.current_label);
                    data_collector_send_alert(alert);
                }

                if (s_collector_mutex) nwy_sdk_mutex_lock(s_collector_mutex, 1000);

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

                if (s_file_is_open) {
                    UINT bw = 0;
                    FRESULT wr_res = f_write(&s_csv_file, csv_row, (UINT)strlen(csv_row), &bw);
                    FRESULT sync_res = f_sync(&s_csv_file); /* Guarantees immediate persistence to NAND flash */
                    if (wr_res != FR_OK || sync_res != FR_OK || bw == 0) {
                        if (!s_alert_sd_error_sent) {
                            s_alert_sd_error_sent = true;
                            char alert[160];
                            snprintf(alert, sizeof(alert),
                                     "[BME688 ALERT] MicroSD write failed (err %d/%d). Recording paused!",
                                     wr_res, sync_res);
                            data_collector_send_alert(alert);
                        }
                    }
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
                nwy_hal_uart_printf(1, LOG_TAG "BME688 scan error code: %d\r\n", rslt);
                if (!s_alert_sensor_error_sent) {
                    s_alert_sensor_error_sent = true;
                    char alert[160];
                    snprintf(alert, sizeof(alert),
                             "[BME688 ALERT] Sensor I2C read error (code %d) on '%s'. Check wiring!",
                             rslt, s_status.current_label);
                    data_collector_send_alert(alert);
                }
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
