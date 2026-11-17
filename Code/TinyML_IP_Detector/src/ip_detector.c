/*
 * ==============================================================================
 * Neoway N706B OpenCPU - Real-Time IP Solution (Isopropanol) Detector
 * ==============================================================================
 * TinyML Chemical Classifier running on ASR1605S (ARM Cortex-R5)
 * Sensor: SmartElex BME688 4-in-1 MEMS Gas Scanner via I2C1
 * Model: Bosch BME AI-Studio Trained Sequential Neural Net (HP-354 Profile)
 * Logic:
 *  - Continuous 10-step HP-354 scanning (~12s per cycle)
 *  - Neural network inference with 99.8% nominal confidence
 *  - Debounce confirmation over 3 consecutive cycles (avoids false alarms)
 *  - Sends STRICTLY ONE SMS alert to user upon confirmed detection
 *  - Highly descriptive, professional UART diagnostic logs
 * ==============================================================================
 */

#include "ip_detector.h"
#include "tinyml_nn.h"
#include "bme68x.h"
#include "i2c_bus.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/pm/nwy_hal_pm.h"
#include "hal/os/nwy_hal_os.h"
#include <stdio.h>
#include <string.h>

#define LOG_TAG "[IP_DETECTOR] "

static bme68x_dev_t      *s_bme_dev = NULL;
static detector_status_t  s_status;
static nwy_osi_thread_t   s_detector_thread = NULL;
static bool               s_thread_running = false;

/* Helper: Render a 30-character ASCII probability bar */
static void format_prob_bar(char *buf, size_t buf_sz, float prob)
{
    if (!buf || buf_sz < 35) return;
    int filled = (int)(prob * 30.0f);
    if (filled < 0) filled = 0;
    if (filled > 30) filled = 30;

    buf[0] = '[';
    for (int i = 0; i < 30; i++) {
        buf[i + 1] = (i < filled) ? '=' : ' ';
    }
    buf[31] = ']';
    buf[32] = '\0';
}

/* Print prominent, high-visibility cycle summary */
static void print_cycle_log(uint32_t cycle, float temp, float pres, float hum,
                            const float r_gas[10], const tinyml_result_t *res)
{
    char bar_air[36];
    char bar_ip[36];
    format_prob_bar(bar_air, sizeof(bar_air), res->class_probabilities[0]);
    format_prob_bar(bar_ip, sizeof(bar_ip), res->class_probabilities[1]);

    nwy_hal_uart_printf(1, "\r\n");
    nwy_hal_uart_printf(1, "+-----------------------------------------------------------------------------+\r\n");
    nwy_hal_uart_printf(1, "| [CYCLE #%04lu] BME688 TINYML GAS INFERENCE ENGINE                            |\r\n", (unsigned long)cycle);
    nwy_hal_uart_printf(1, "| Temp: %5.2f C  |  Pres: %6.2f hPa  |  Hum: %5.2f %%  |  Profile: HP-354 (140ms)|\r\n", temp, pres, hum);
    nwy_hal_uart_printf(1, "+-----------------------------------------------------------------------------+\r\n");
    nwy_hal_uart_printf(1, "| Gas Steps (kOhm):                                                           |\r\n");
    nwy_hal_uart_printf(1, "|  S0: %6.1f | S1: %6.1f | S2: %6.1f | S3: %6.1f | S4: %6.1f              |\r\n",
                        r_gas[0] / 1000.0f, r_gas[1] / 1000.0f, r_gas[2] / 1000.0f,
                        r_gas[3] / 1000.0f, r_gas[4] / 1000.0f);
    nwy_hal_uart_printf(1, "|  S5: %6.1f | S6: %6.1f | S7: %6.1f | S8: %6.1f | S9: %6.1f              |\r\n",
                        r_gas[5] / 1000.0f, r_gas[6] / 1000.0f, r_gas[7] / 1000.0f,
                        r_gas[8] / 1000.0f, r_gas[9] / 1000.0f);
    nwy_hal_uart_printf(1, "+-----------------------------------------------------------------------------+\r\n");
    nwy_hal_uart_printf(1, "| Neural Network Classification Probabilities:                                |\r\n");
    nwy_hal_uart_printf(1, "|   NORMAL AIR  : %5.1f %%  %s                           |\r\n", res->class_probabilities[0] * 100.0f, bar_air);
    nwy_hal_uart_printf(1, "|   IP SOLUTION : %5.1f %%  %s                           |\r\n", res->class_probabilities[1] * 100.0f, bar_ip);
    nwy_hal_uart_printf(1, "+-----------------------------------------------------------------------------+\r\n");

    if (res->predicted_class == CLASS_IP_SOLUTION && res->confidence >= IP_CONFIDENCE_THRESHOLD) {
        if (s_status.consecutive_ip_cycles >= IP_CONFIRM_CYCLES) {
            nwy_hal_uart_printf(1, "| STATUS: [*** CRITICAL ALERT ***] IP SOLUTION CONFIRMED (Latch: ACTIVE)     |\r\n");
        } else {
            nwy_hal_uart_printf(1, "| STATUS: [!] IP SOLUTION DETECTED (Confirming: %lu/%d Cycles)                |\r\n",
                                (unsigned long)s_status.consecutive_ip_cycles, IP_CONFIRM_CYCLES);
        }
    } else {
        nwy_hal_uart_printf(1, "| STATUS: [OK] Normal Ambient Air Detected                                    |\r\n");
    }
    nwy_hal_uart_printf(1, "+-----------------------------------------------------------------------------+\r\n");
}

/* Dispatch strictly ONE SMS Alert when IP Solution is confirmed */
static void dispatch_single_sms_alert(float confidence, float temp, float hum)
{
    if (s_status.sms_alert_sent) {
        return; /* Strictly enforced: only one SMS alert per detection event */
    }

    nwy_hal_uart_printf(1, "\r\n===============================================================================\r\n");
    nwy_hal_uart_printf(1, ">>> [CRITICAL EVENT] IP SOLUTION CONFIRMED! DISPATCHING ONE-TIME SMS... <<<\r\n");
    nwy_hal_uart_printf(1, ">>> Destination Phone: %s\r\n", s_status.alert_phone);

    char sms_body[160];
    snprintf(sms_body, sizeof(sms_body),
             "CRITICAL ALERT: IP Solution (Isopropanol) CONFIRMED on Neoway N706B! Conf: %.1f%%, T: %.1fC, RH: %.1f%%. Gas Scanner Unit 1.",
             confidence * 100.0f, temp, hum);

    bool ok = nwy_hal_sms_send(1, s_status.alert_phone, sms_body);

    if (ok) {
        s_status.sms_alert_sent = true;
        s_status.sms_alert_timestamp = (uint32_t)(nwy_uptime_get() / 1000);
        nwy_hal_uart_printf(1, ">>> SMS STATUS: DELIVERED TO CELLULAR CARRIER [SUCCESS]\r\n");
        nwy_hal_uart_printf(1, ">>> ALERT LATCH ENGAGED: Strictly 1 SMS sent. Continuous monitoring continues.\r\n");
    } else {
        nwy_hal_uart_printf(1, ">>> SMS STATUS: FAILED TO TRANSMIT! Network or SIM error.\r\n");
    }
    nwy_hal_uart_printf(1, "===============================================================================\r\n\r\n");
}

/* Main Background Detection Loop */
static void detector_worker_task(void *param)
{
    (void)param;
    nwy_hal_uart_printf(1, LOG_TAG "Background worker thread started. Beginning HP-354 scans...\r\n");

    /* Initial 3-second stabilization pause */
    nwy_hal_os_thread_sleep(3000);

    while (s_thread_running) {
        float r_gas[TINYML_NUM_STEPS];
        float temp = 0.0f, hum = 0.0f, pres = 0.0f;

        /* Execute 10-step HP-354 Multi-Heater Gas Scan (~10.8 seconds) */
        int8_t rslt = bme68x_perform_10step_scan(s_bme_dev, r_gas, &temp, &hum, &pres);

        if (rslt != BME68X_OK) {
            nwy_hal_uart_printf(1, LOG_TAG "Warning: Sensor scan returned error %d. Retrying...\r\n", rslt);
            nwy_hal_os_thread_sleep(2000);
            continue;
        }

        s_status.cycle_count++;
        s_status.last_temp = temp;
        s_status.last_pres = pres;
        s_status.last_hum = hum;

        /* Run TinyML Neural Network Forward Pass */
        tinyml_result_t nn_result;
        if (tinyml_nn_predict(r_gas, &nn_result)) {
            s_status.last_normal_confidence = nn_result.class_probabilities[0];
            s_status.last_ip_confidence = nn_result.class_probabilities[1];

            /* Debounce Logic: Require 3 consecutive cycles above threshold */
            if (nn_result.predicted_class == CLASS_IP_SOLUTION &&
                nn_result.confidence >= IP_CONFIDENCE_THRESHOLD) {
                s_status.consecutive_ip_cycles++;

                if (s_status.consecutive_ip_cycles >= IP_CONFIRM_CYCLES) {
                    s_status.state = DETECTOR_STATE_ALERT_LATCHED;
                    /* Confirmed IP Solution -> Dispatch strictly ONE SMS */
                    dispatch_single_sms_alert(nn_result.confidence, temp, hum);
                } else {
                    s_status.state = DETECTOR_STATE_CONFIRMING;
                }
            } else {
                /* Reset debounce counter if reading returns to normal */
                if (s_status.consecutive_ip_cycles > 0) {
                    nwy_hal_uart_printf(1, LOG_TAG "Air normalized. Resetting confirmation counter (%lu -> 0).\r\n",
                                        (unsigned long)s_status.consecutive_ip_cycles);
                }
                s_status.consecutive_ip_cycles = 0;
                if (!s_status.sms_alert_sent) {
                    s_status.state = DETECTOR_STATE_MONITORING;
                }
            }

            /* Print detailed UART log */
            print_cycle_log(s_status.cycle_count, temp, pres, hum, r_gas, &nn_result);
        }

        /* Rest between cycles (~1.2 seconds) */
        nwy_hal_os_thread_sleep(1200);
    }

    nwy_hal_uart_printf(1, LOG_TAG "Background worker thread exited.\r\n");
    nwy_thread_suspend(NULL);
}

bool ip_detector_init(bme68x_dev_t *bme_dev)
{
    s_bme_dev = bme_dev;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = DETECTOR_STATE_IDLE;
    strncpy(s_status.alert_phone, DEFAULT_ALERT_PHONE, sizeof(s_status.alert_phone) - 1);

    tinyml_nn_init();

    if (!s_bme_dev || s_bme_dev->chip_id != BME68X_CHIP_ID) {
        nwy_hal_uart_printf(1, LOG_TAG "ERROR: SmartElex BME688 sensor not ready (0x76)!\r\n");
        return false;
    }

    nwy_hal_uart_printf(1, LOG_TAG "SmartElex BME688 linked successfully (Chip ID: 0x%02X)\r\n", s_bme_dev->chip_id);
    nwy_hal_uart_printf(1, LOG_TAG "TinyML Neural Network loaded: 2 Classes (NORMAL AIR, IP SOLUTION)\r\n");
    nwy_hal_uart_printf(1, LOG_TAG "Alert SMS Destination: %s (Policy: Strictly ONE SMS on Confirmation)\r\n", s_status.alert_phone);

    return true;
}

bool ip_detector_start(void)
{
    if (s_thread_running) return true;

    s_thread_running = true;
    s_status.state = DETECTOR_STATE_MONITORING;

    nwy_error_e ret = nwy_thread_create(&s_detector_thread, "ip_detect", 50,
                                        detector_worker_task, NULL,
                                        10, 1024 * 16, NULL);
    return (ret == NWY_SUCCESS);
}

void ip_detector_stop(void)
{
    s_thread_running = false;
    s_status.state = DETECTOR_STATE_IDLE;
}

void ip_detector_get_status(detector_status_t *status)
{
    if (status) {
        memcpy(status, &s_status, sizeof(detector_status_t));
    }
}

void ip_detector_reset_alert(void)
{
    s_status.sms_alert_sent = false;
    s_status.consecutive_ip_cycles = 0;
    s_status.state = DETECTOR_STATE_MONITORING;
    nwy_hal_uart_printf(1, LOG_TAG "SMS Alert latch RESET by user command. Armed for next event.\r\n");
}

void ip_detector_handle_sms_command(const char *sender_phone, const char *msg_body)
{
    if (!sender_phone || !msg_body) return;

    nwy_hal_uart_printf(1, LOG_TAG "Incoming SMS from %s: \"%s\"\r\n", sender_phone, msg_body);

    char reply[160];
    reply[0] = '\0';

    if (strstr(msg_body, "STATUS") || strstr(msg_body, "status")) {
        snprintf(reply, sizeof(reply),
                 "STATUS: Cycle #%lu. IP Conf: %.1f%%, Air Conf: %.1f%%. State: %s. SMS Sent: %s",
                 (unsigned long)s_status.cycle_count,
                 s_status.last_ip_confidence * 100.0f,
                 s_status.last_normal_confidence * 100.0f,
                 (s_status.state == DETECTOR_STATE_ALERT_LATCHED) ? "CONFIRMED_IP" : "NORMAL_AIR",
                 s_status.sms_alert_sent ? "YES" : "NO");
    } else if (strstr(msg_body, "RESET") || strstr(msg_body, "reset")) {
        ip_detector_reset_alert();
        snprintf(reply, sizeof(reply), "RESET: IP Alert Latch cleared. System re-armed.");
    }

    if (reply[0] != '\0') {
        nwy_hal_uart_printf(1, LOG_TAG "Sending SMS reply to %s: \"%s\"\r\n", sender_phone, reply);
        nwy_hal_sms_send(1, sender_phone, reply);
    }
}
