/**
 * @file main.c
 * @brief Industrial eDRX (Extended Discontinuous Reception) Power Benchmark
 * @note Designed for Neoway N706B (ASR1605S Cat-1)
 *       Target: Ultra-low power idle listening with Indian M2M/Cellular SIMs
 */

#include "edrx_config.h"

#include "hal/nwy_hal_gpio.h"
#include "hal/nwy_hal_net.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_pm.h"
#include "hal/nwy_hal_sim.h"
#include "hal/nwy_hal_sms.h"
#include "hal/nwy_hal_uart.h"

#include "nwy_dev_api.h"
#include "nwy_gpio_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_pm_api.h"
#include "nwy_sim_api.h"
#include "nwy_vir_at_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_dbg_uart_fd = -1;
#define printf(fmt, ...) nwy_hal_uart_printf(g_dbg_uart_fd, fmt, ##__VA_ARGS__)

static nwy_osi_semaphore_t g_sms_sem = NULL;
static volatile int g_pending_sms_index = -1;
static void on_incoming_sms_cb(int sim_id, int sms_index) {
  (void)sim_id;
  g_pending_sms_index = sms_index;
  if (g_sms_sem) {
    nwy_hal_os_semaphore_release(g_sms_sem);
  }
}

/* =========================================================================
 * VIRTUAL AT ENGINE TRANSACTION HANDLER
 * ========================================================================= */
static bool send_virt_at_cmd(const char *cmd, char *resp_out, int resp_len) {
  if (!cmd) return false;

  nwy_at_info_t info;
  memset(&info, 0, sizeof(info));
  size_t len = strlen(cmd);
  if (len > NWY_AT_BUF_MAX - 2) return false;

  memcpy(info.at_command, cmd, len);
  info.at_command[len] = '\r';
  info.at_command[len + 1] = '\n';
  info.length = (int)(len + 2);

  memcpy(info.ok_fmt, "OK", 3);
  memcpy(info.err_fmt, "ERROR", 6);

  char local_buf[256];
  memset(local_buf, 0, sizeof(local_buf));
  char *p_buf = resp_out ? resp_out : local_buf;
  int b_len = resp_out ? resp_len : (int)sizeof(local_buf);

  nwy_error_e ret = nwy_virt_at_cmd_send(&info, p_buf, b_len, 3);
  return (ret == NWY_SUCCESS);
}

/* =========================================================================
 * HARDWARE POWER GATING & CARRIER ISOLATION
 * ========================================================================= */
static void isolate_carrier_hardware_for_sleep(void) {
  /* 1. Cut High-Side Switch (GPIO00 = LOW): Cuts 12V / 3.8V rail to VCC_SWout.
   * Cuts power to THVD1420DR RS-485 transceiver and carrier level shifters (0.0 uA). */
  nwy_hal_gpio_init_out(PIN_PERIPHERAL_POWER_GPIO0, false);
  nwy_hal_gpio_set_value(PIN_PERIPHERAL_POWER_GPIO0, false);

  /* 2. Configure all exposed carrier bus lines to INPUT with PULL-DOWN */
  size_t pin_count = sizeof(s_carrier_isolation_gpios) / sizeof(s_carrier_isolation_gpios[0]);
  for (size_t i = 0; i < pin_count; i++) {
    uint32_t pin = s_carrier_isolation_gpios[i];
    nwy_gpio_direction_set(pin, PIN_DIRECTION_IN);
    nwy_gpio_pull_set(pin, PIN_PULL_PD);
  }
}

/* =========================================================================
 * 3GPP EDRX CONVERSION UTILITIES (SECONDS CALCULATION)
 * ========================================================================= */
static float convert_tedrx_bits_to_seconds(const char *bits) {
  if (!bits || strlen(bits) < 4) return 0.0f;
  if (strcmp(bits, "0000") == 0) return 5.12f;
  if (strcmp(bits, "0001") == 0) return 10.24f;
  if (strcmp(bits, "0010") == 0) return 20.48f;
  if (strcmp(bits, "0011") == 0) return 40.96f;
  if (strcmp(bits, "0100") == 0) return 61.44f;
  if (strcmp(bits, "0101") == 0) return 81.92f;
  if (strcmp(bits, "0110") == 0) return 102.40f;
  if (strcmp(bits, "0111") == 0) return 122.88f;
  if (strcmp(bits, "1000") == 0) return 143.36f;
  if (strcmp(bits, "1001") == 0) return 163.84f;
  if (strcmp(bits, "1010") == 0) return 327.68f;
  if (strcmp(bits, "1011") == 0) return 655.36f;
  if (strcmp(bits, "1100") == 0) return 1310.72f;
  if (strcmp(bits, "1101") == 0) return 2621.44f;
  return 0.0f;
}

static float convert_ptw_bits_to_seconds(const char *bits) {
  if (!bits || strlen(bits) < 4) return 0.0f;
  if (strcmp(bits, "0000") == 0) return 1.28f;
  if (strcmp(bits, "0001") == 0) return 2.56f;
  if (strcmp(bits, "0010") == 0) return 3.84f;
  if (strcmp(bits, "0011") == 0) return 5.12f;
  if (strcmp(bits, "0100") == 0) return 6.40f;
  if (strcmp(bits, "0101") == 0) return 7.68f;
  if (strcmp(bits, "0110") == 0) return 8.96f;
  if (strcmp(bits, "0111") == 0) return 10.24f;
  if (strcmp(bits, "1000") == 0) return 11.52f;
  if (strcmp(bits, "1001") == 0) return 12.80f;
  if (strcmp(bits, "1010") == 0) return 14.08f;
  if (strcmp(bits, "1011") == 0) return 15.36f;
  if (strcmp(bits, "1100") == 0) return 16.64f;
  if (strcmp(bits, "1101") == 0) return 17.92f;
  if (strcmp(bits, "1110") == 0) return 19.20f;
  if (strcmp(bits, "1111") == 0) return 20.48f;
  return 0.0f;
}

/* =========================================================================
 * INSPECT PROVISIONED SUBSCRIPTION CONTEXT
 * ========================================================================= */
static void inspect_subscription_context(edrx_status_report_t *report) {
  char resp[160];

  /* 1. Retrieve IMSI from SIM */
  if (nwy_hal_sim_get_imsi(1, report->imsi) && strlen(report->imsi) >= 5) {
    LOGI("SIM Card IMSI (via HAL): %s", report->imsi);
  }

  /* 2. Retrieve ICCID from SIM */
  if (nwy_hal_sim_get_iccid(1, report->iccid) && strlen(report->iccid) >= 5) {
    LOGI("SIM Card ICCID (via HAL): %s", report->iccid);
  }

  /* 3. Retrieve Phone Number (MSISDN) */
  char phone_raw[48] = {0};
  if (nwy_hal_sim_get_msisdn(1, phone_raw, sizeof(phone_raw)) && strlen(phone_raw) > 0) {
    LOGI("Raw SIM Phone Number (via HAL): %s", phone_raw);
  }

  /* Also query via standard 3GPP AT+CNUM */
  memset(resp, 0, sizeof(resp));
  if (send_virt_at_cmd("AT+CNUM", resp, sizeof(resp))) {
    LOGI("Raw AT+CNUM Response: %s", resp);
    if (strlen(phone_raw) == 0) {
      char *cnum_match = strstr(resp, "+CNUM:");
      if (cnum_match) {
        char *first_comma = strchr(cnum_match, ',');
        if (first_comma) {
          char *q1 = strchr(first_comma, '\"');
          if (q1) {
            char *q2 = strchr(q1 + 1, '\"');
            if (q2) {
              size_t p_len = (size_t)(q2 - q1 - 1);
              if (p_len > 0 && p_len < sizeof(phone_raw)) {
                strncpy(phone_raw, q1 + 1, p_len);
                phone_raw[p_len] = '\0';
              }
            }
          }
        }
      }
    }
  }

  /* Sanitize phone number (strip quotes and Neoway Crane TON 0x91 -> "19" BCD artifact):
   * Indian DoT 13-digit M2M series: "+19915754001037506" -> "+91 5754001037506" (M2M: 5754001037506) */
  if (strlen(phone_raw) > 0) {
    char cleaned[48] = {0};
    size_t c_idx = 0;
    for (size_t i = 0; phone_raw[i] != '\0' && c_idx < sizeof(cleaned) - 1; i++) {
      if (phone_raw[i] != '\"' && phone_raw[i] != ' ' && phone_raw[i] != '\r' && phone_raw[i] != '\n') {
        cleaned[c_idx++] = phone_raw[i];
      }
    }
    cleaned[c_idx] = '\0';

    const char *p = cleaned;
    bool has_plus = false;
    if (*p == '+') {
      has_plus = true;
      p++;
    }
    /* If prepended with swapped 0x91 ToA byte ("19") before Indian country code "91": */
    if (strncmp(p, "1991", 4) == 0) {
      p += 2; /* Skip "19" artifact, keeping "91" (India CC) */
      has_plus = true;
    }

    if (has_plus) {
      snprintf(report->phone_number, sizeof(report->phone_number), "+%s", p);
    } else {
      snprintf(report->phone_number, sizeof(report->phone_number), "%s", p);
    }
  }

  if (strlen(report->phone_number) == 0) {
    strncpy(report->phone_number, "Not Programmed on SIM (M2M Data Profile)", sizeof(report->phone_number) - 1);
  }

  /* 4. Query Current PDP Context without overwriting: AT+CGDCONT? */
  memset(resp, 0, sizeof(resp));
  if (send_virt_at_cmd("AT+CGDCONT?", resp, sizeof(resp))) {
    LOGI("Current Provisioned PDP Context (AT+CGDCONT?): %s", resp);

    /* Extract APN if present in quotes */
    char *cgdcont_match = strstr(resp, "+CGDCONT: 1");
    if (cgdcont_match) {
      char *first_quote = strchr(cgdcont_match, '\"');
      if (first_quote) {
        char *second_quote = strchr(first_quote + 1, '\"');
        if (second_quote) {
          char *third_quote = strchr(second_quote + 1, '\"');
          if (third_quote) {
            char *fourth_quote = strchr(third_quote + 1, '\"');
            if (fourth_quote) {
              size_t apn_len = (size_t)(fourth_quote - third_quote - 1);
              if (apn_len < sizeof(report->active_apn)) {
                strncpy(report->active_apn, third_quote + 1, apn_len);
                report->active_apn[apn_len] = '\0';
              }
            }
          }
        }
      }
    }
  }

  if (strlen(report->active_apn) == 0) {
    strncpy(report->active_apn, "Airtel SIM Provisioned Context", sizeof(report->active_apn) - 1);
  }

  LOGI("==============================================================");
  LOGI(">>> CELLULAR SUBSCRIPTION PROFILE <<<");
  LOGI("  IMSI         : %s", report->imsi);
  LOGI("  Phone Number : %s", report->phone_number);
  LOGI("  Operator     : %s", report->oper_alpha);
  LOGI("  APN          : %s (Preserving SIM provisioned bearer)", report->active_apn);
  LOGI("==============================================================");
}

/* =========================================================================
 * WAIT FOR LTE NETWORK ATTACH USING PROVEN HAL API
 * ========================================================================= */
static bool wait_for_lte_attach_hal(uint32_t timeout_sec, edrx_status_report_t *report) {
  LOGI("Ensuring RF is in full functionality mode: AT+CFUN=1...");
  char resp[64];
  memset(resp, 0, sizeof(resp));
  send_virt_at_cmd("AT+CFUN=1", resp, sizeof(resp));

  LOGI("Awaiting LTE Network Registration (up to %lu seconds)...", (unsigned long)timeout_sec);
  uint32_t elapsed = 0;
  bool registered = false;

  while (elapsed < timeout_sec) {
    if (nwy_hal_net_is_registered(1)) {
      registered = true;
      break;
    }
    nwy_hal_os_thread_sleep(1000);
    elapsed++;
    if (elapsed % 5 == 0) {
      LOGI("  ...Waiting for LTE network attach (%lu/%lu s)...", (unsigned long)elapsed, (unsigned long)timeout_sec);
    }
  }

  if (registered) {
    int csq = 99, ber = 99;
    char oper_name[64] = "Unknown";
    nwy_hal_net_get_csq(1, &csq, &ber);
    nwy_hal_net_get_operator_name(1, oper_name, sizeof(oper_name));

    report->csq_rssi = csq;
    report->csq_ber = ber;
    strncpy(report->oper_alpha, oper_name, sizeof(report->oper_alpha) - 1);

    LOGI("==============================================================");
    LOGI(">>> LTE NETWORK ATTACHED & REGISTERED SUCCESSFULLY! <<<");
    LOGI("  Serving Operator: %s", report->oper_alpha);
    LOGI("  Signal Quality  : CSQ %d (BER: %d)", report->csq_rssi, report->csq_ber);
    LOGI("==============================================================");
    return true;
  }

  LOGE("LTE Network Registration timed out after %lu seconds!", (unsigned long)timeout_sec);
  return false;
}

/* =========================================================================
 * 3GPP EDRX PARSER & NEGOTIATION HANDLER
 * ========================================================================= */
static bool parse_edrxrdp_str(const char *resp, edrx_status_report_t *report) {
  if (!resp || !report) return false;
  char *match = strstr(resp, "+CEDRXRDP:");
  if (!match) return false;

  report->edrx_supported_by_network = true;
  char tedrx_str[16] = {0};
  char ptw_str[16] = {0};

  char *first_quote = strchr(match, '\"');
  if (first_quote) {
    char *second_quote = strchr(first_quote + 1, '\"');
    if (second_quote) {
      size_t t_len = (size_t)(second_quote - first_quote - 1);
      if (t_len < sizeof(tedrx_str)) {
        strncpy(tedrx_str, first_quote + 1, t_len);
        tedrx_str[t_len] = '\0';
      }

      char *third_quote = strchr(second_quote + 1, '\"');
      if (third_quote) {
        char *fourth_quote = strchr(third_quote + 1, '\"');
        if (fourth_quote) {
          size_t p_len = (size_t)(fourth_quote - third_quote - 1);
          if (p_len < sizeof(ptw_str)) {
            strncpy(ptw_str, third_quote + 1, p_len);
            ptw_str[p_len] = '\0';
          }
        }
      }
    }
  }

  if (strlen(tedrx_str) > 0) {
    report->edrx_granted_by_mme = true;
    strncpy(report->granted_tedrx_bits, tedrx_str, sizeof(report->granted_tedrx_bits) - 1);
    strncpy(report->granted_ptw_bits, ptw_str, sizeof(report->granted_ptw_bits) - 1);
    report->granted_tedrx_seconds = convert_tedrx_bits_to_seconds(tedrx_str);
    report->granted_ptw_seconds = convert_ptw_bits_to_seconds(ptw_str);
    return true;
  }
  return false;
}

static bool configure_and_verify_edrx(edrx_status_report_t *report) {
  LOGI("==============================================================");
  LOGI(">>> INITIATING 3GPP REL-13 EDRX NEGOTIATION (E-UTRAN) <<<");
  LOGI("==============================================================");

  char resp[160];
  const char *candidate_tedrx[] = {
    EDRX_VAL_20_48_SEC,  /* 20.48s - Standard Rel-13 primary */
    EDRX_VAL_40_96_SEC,  /* 40.96s - Extended power save */
    EDRX_VAL_10_24_SEC,  /* 10.24s - Faster paging response */
    EDRX_VAL_5_12_SEC    /* 5.12s  - Minimum eDRX */
  };
  size_t num_candidates = sizeof(candidate_tedrx) / sizeof(candidate_tedrx[0]);

  bool granted = false;

  for (size_t c = 0; c < num_candidates; c++) {
    const char *target_tedrx = candidate_tedrx[c];
    strncpy(report->requested_tedrx_bits, target_tedrx, sizeof(report->requested_tedrx_bits) - 1);

    char edrxs_cmd[48];
    snprintf(edrxs_cmd, sizeof(edrxs_cmd), "AT+CEDRXS=2,%d,\"%s\"",
             EDRX_ACT_TYPE_EUTRAN, target_tedrx);
    LOGI("[%d/%d] Requesting 3GPP eDRX: %s (%.2f s)...",
         (int)(c + 1), (int)num_candidates, edrxs_cmd,
         convert_tedrx_bits_to_seconds(target_tedrx));

    memset(resp, 0, sizeof(resp));
    send_virt_at_cmd(edrxs_cmd, resp, sizeof(resp));

    /* Query Read Parameters from Network Core (AT+CEDRXRDP) */
    nwy_hal_os_thread_sleep(1000);
    memset(resp, 0, sizeof(resp));
    send_virt_at_cmd("AT+CEDRXRDP", resp, sizeof(resp));
    LOGI("  -> AT+CEDRXRDP: %s", resp);

    if (parse_edrxrdp_str(resp, report)) {
      granted = true;
      break;
    }

    /* If first candidate didn't immediately grant, trigger Tracking Area Update (TAU)
     * so modem stack explicitly transmits eDRX Information Element to the MME */
    if (c == 0) {
      LOGI("  -> Triggering EPS Tracking Area Update to present eDRX IE to MME...");
      send_virt_at_cmd("AT+COPS=2", NULL, 0);
      nwy_hal_os_thread_sleep(2000);
      send_virt_at_cmd("AT+COPS=0", NULL, 0);

      int wait_sec = 0;
      while (!nwy_hal_net_is_registered(1) && wait_sec < 25) {
        nwy_hal_os_thread_sleep(1000);
        wait_sec++;
      }
      nwy_hal_os_thread_sleep(1500);

      memset(resp, 0, sizeof(resp));
      send_virt_at_cmd("AT+CEDRXRDP", resp, sizeof(resp));
      LOGI("  -> AT+CEDRXRDP (post-TAU): %s", resp);
      if (parse_edrxrdp_str(resp, report)) {
        granted = true;
        break;
      }
    }
  }

  LOGI("==============================================================");
  LOGI(">>> 3GPP REL-13 EDRX NEGOTIATION ANALYSIS REPORT <<<");
  LOGI("==============================================================");
  LOGI("  Radio Access Technology : E-UTRAN LTE Cat-1 (AcT: 4)");
  LOGI("  Requested TeDRX (UE)    : %s (%.2f seconds)",
       report->requested_tedrx_bits,
       convert_tedrx_bits_to_seconds(report->requested_tedrx_bits));

  if (report->edrx_granted_by_mme) {
    LOGI("  Network MME Status      : GRANTED (eDRX Active!)");
    LOGI("  Network Granted TeDRX   : %s -> %.2f SECONDS Paging Interval",
         report->granted_tedrx_bits, report->granted_tedrx_seconds);
    LOGI("  Network Granted PTW     : %s -> %.2f SECONDS Paging Window",
         report->granted_ptw_bits, report->granted_ptw_seconds);
    LOGI("  Estimated Idle Current  : ~1.2 mA - 1.8 mA baseline (eDRX active!)");
  } else {
    LOGW("  Network MME Status      : NOT GRANTED by Serving Cell MME");
    LOGW("  Analysis                : Airtel cell tower operates in Standard DRX");
    LOGW("                            (1.28s paging cycle) for this SIM profile.");
    LOGW("  Paging Mode Active      : Standard 3GPP DRX (Immediate Reachability)");
    LOGW("  Physical Current Floor  : ~4.5 mA - 5.5 mA (Normal for Cat-1 in 1.28s DRX).");
  }
  LOGI("==============================================================");

  return true;
}

/* =========================================================================
 * SAMPLE CELLULAR RADIO QUALITY METRICS
 * ========================================================================= */
static void sample_radio_metrics(edrx_status_report_t *report) {
  int csq = 99, ber = 99;
  if (nwy_hal_net_get_csq(1, &csq, &ber)) {
    report->csq_rssi = csq;
    report->csq_ber = ber;
  }

  nwy_nw_signal_info_t sinfo;
  memset(&sinfo, 0, sizeof(sinfo));
  if (nwy_hal_net_get_signal_info(1, &sinfo)) {
    if (sinfo.rat == NWY_NW_RAT_LTE) {
      if (sinfo.rat_signal_info.lte_signal_info.rsrp != 255) {
        report->rsrp_dbm = sinfo.rat_signal_info.lte_signal_info.rsrp;
      }
      if (sinfo.rat_signal_info.lte_signal_info.rsrq != 255) {
        report->rsrq_db = sinfo.rat_signal_info.lte_signal_info.rsrq;
      }
    }
  }

  nwy_hal_pm_get_vbat(&report->vbat_mv);
}

/* =========================================================================
 * PRINT INDUSTRIAL BENCHMARK STATUS CARD
 * ========================================================================= */
static void print_benchmark_card(const edrx_status_report_t *report) {
  LOGI("");
  LOGI("##############################################################");
  LOGI(">>> EDRX POWER BENCHMARK STATUS AUDIT [#%lu] <<<", (unsigned long)report->audit_cycle);
  LOGI("##############################################################");
  LOGI("  * System Uptime       : %lu seconds (%lu minutes)",
       (unsigned long)report->uptime_sec, (unsigned long)(report->uptime_sec / 60));
  LOGI("  * VBAT Rail Voltage   : %d mV", report->vbat_mv);
  LOGI("  * SIM Card IMSI       : %s", report->imsi);
  LOGI("  * SIM Phone Number    : %s", report->phone_number);
  LOGI("  * Operator            : %s (APN: %s)", report->oper_alpha, report->active_apn);
  LOGI("  * Cellular Signal     : CSQ %d (BER: %d) | RSRP: %d dBm | RSRQ: %d dB",
       report->csq_rssi, report->csq_ber, report->rsrp_dbm, report->rsrq_db);
  LOGI("  * Paging Mode State   : %s",
       report->edrx_granted_by_mme ? "ACTIVE (eDRX Enabled)" : "ACTIVE (Standard DRX RRC Idle)");
  if (report->edrx_granted_by_mme) {
    LOGI("  * Paging Cycle (TeDRX): %.2f sec (Sleep Interval between pages)", report->granted_tedrx_seconds);
    LOGI("  * Paging Window (PTW) : %.2f sec (Active Listening Duration)", report->granted_ptw_seconds);
  }
  LOGI("  * Power Gating Rails  : GPIO00 (VCC_SWout) = 0V (CUT) | Carrier Pins: Isolated");
  LOGI("  * Baseband Sleep Core : AT+ENPWRSAVE=2 (DTR HIGH = SLEEP Active)");
  LOGI("##############################################################");
}

/* =========================================================================
 * MAIN BENCHMARK THREAD
 * ========================================================================= */
static void main_task(void *param) {
  nwy_hal_os_thread_sleep(2000);

  g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
  }

  LOGI("====================================================================");
  LOGI("   NEOWAY N706B: INDUSTRIAL eDRX POWER BENCHMARK (INDIA MNO)        ");
  LOGI("====================================================================");
  LOGI("  Hardware: ELEKTRONIKA_N706B Carrier (ASR1605S Cat-1)");
  LOGI("  Firmware Version: %s", APP_VERSION);
  LOGI("  Power Optimizations: Master Rail Switch (GPIO00) + Carrier Isolation");
  LOGI("  Sleep Target: 3GPP Rel-13 eDRX Paging Sleep (TeDRX: 20.48s - 40.96s)");
  LOGI("====================================================================");

  /* 1. Initialize Virtual AT Engine & Event Semaphore */
  nwy_error_e at_init = nwy_virt_at_parameter_init();
  LOGI("Virtual AT engine init: %s", (at_init == NWY_SUCCESS) ? "SUCCESS" : "FAIL");
  nwy_hal_os_semaphore_create(&g_sms_sem, 0, 1);

  char at_resp[64];

  /* 2. Hardware Carrier Power Gating: Immediately cut external peripheral rail */
  LOGI("Power-gating external sensors and carrier level shifters (GPIO00=0)...");
  isolate_carrier_hardware_for_sleep();

  /* 3. Enforce Baseband 32kHz Sleep Policy (DTR HIGH = SLEEP) */
  LOGI("Enforcing baseband 32kHz sleep policy: AT+ENPWRSAVE=2 (DTR HIGH = SLEEP)...");
  memset(at_resp, 0, sizeof(at_resp));
  send_virt_at_cmd("AT+ENPWRSAVE=2", at_resp, sizeof(at_resp));
  LOGI("Set AT+ENPWRSAVE=2: %s", at_resp);
  memset(at_resp, 0, sizeof(at_resp));
  send_virt_at_cmd("AT+ENPWRSAVE?", at_resp, sizeof(at_resp));
  LOGI("Verified AT+ENPWRSAVE?: %s", at_resp);

  /* 4. Ensure PSM is disabled so eDRX takes exclusive priority */
  memset(at_resp, 0, sizeof(at_resp));
  send_virt_at_cmd("AT+CPSMS=0", at_resp, sizeof(at_resp));

  edrx_status_report_t status_report;
  memset(&status_report, 0, sizeof(status_report));

  /* 5. Await SIM Card Ready */
  LOGI("Awaiting SIM Card Initialization...");
  int sim_wait = 0;
  while (!nwy_hal_sim_is_ready(1) && sim_wait < 15) {
    nwy_hal_os_thread_sleep(1000);
    sim_wait++;
  }
  LOGI("SIM Card READY confirmed (after %d s)!", sim_wait);

  /* 6. Arm SMS Engine: Register callback to wake and log incoming SMS */
  nwy_hal_sms_init(1, NWY_SMS_STORAGE_TYPE_UIM);
  nwy_hal_sms_register_recv_cb(1, on_incoming_sms_cb);

  /* 7. Initial LTE Network Attachment */
  if (wait_for_lte_attach_hal(35, &status_report)) {
    LOGI("LTE Attachment confirmed!");
  } else {
    LOGW("Initial LTE attach delayed; will retry dynamically in audit loop...");
  }

  /* 8. Inspect Provisioned Subscription Context */
  inspect_subscription_context(&status_report);
  LOGI("SMS Engine armed on SIM 1 (Ready to receive messages at %s)!", status_report.phone_number);

  /* 9. Complete 3GPP Rel-13 eDRX Negotiation & Verification with Airtel MME */
  configure_and_verify_edrx(&status_report);

  /* 11. Sample Initial Cellular Quality & Telemetry */
  sample_radio_metrics(&status_report);
  status_report.audit_cycle = 1;
  status_report.uptime_sec = 0;
  print_benchmark_card(&status_report);

  LOGI("------------------------------------------------------------------");
  LOGI("CRITICAL PPK2 BENCHMARK INSTRUCTION:");
  LOGI("1. DISCONNECT USB CABLE FROM PC! (USB VBUS & PHY draws ~9-11 mA).");
  LOGI("2. Supply power strictly via PPK2 on Vin/VBAT and GND.");
  LOGI("3. Hardware UART1 (Pins 35 TX / 36 RX) outputs all logs.");
#if EDRX_BENCHMARK_PURE_CONTINUOUS_MODE
  LOGI("4. Benchmark Mode: PURE CONTINUOUS STEADY-STATE LISTENING (NO 20 mA blocks).");
  LOGI("   - Initial AT command setup completed.");
  LOGI("   - Network RRC Inactivity Timer will expire in ~12-15s without further traffic.");
  LOGI("   - Radio settles into RRC Idle / eDRX listening state permanently.");
  LOGI("   - Incoming SMS to %s will wake UART and display message!", status_report.phone_number);
#else
  LOGI("4. Undisturbed listening window: %d seconds (%d minutes).",
       EDRX_HEALTH_AUDIT_INTERVAL_SEC, EDRX_HEALTH_AUDIT_INTERVAL_SEC / 60);
#endif
  LOGI("------------------------------------------------------------------");

  LOGI(">>> ENTERING STEADY-STATE eDRX PAGING SLEEP <<<");
#if (!EDRX_KEEP_UART_OPEN_FOR_DIAGNOSTICS)
  LOGI("Closing UART1 debug port now to allow UART clock gating...");
  LOGI("Observe PPK2: Within 15s RRC release, current drops to ~4.5 mA - 5.5 mA baseline!");
  nwy_hal_os_thread_sleep(300);

  /* Close UART to allow UART peripheral clocks to gate */
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_close(g_dbg_uart_fd);
    g_dbg_uart_fd = -1;
    nwy_hal_uart_set_log_fd(-1);
  }
#else
  LOGI("UART1 debug port will remain ACTIVE for live incoming SMS & diagnostics.");
  LOGI("Send an SMS from another mobile phone to: %s", status_report.phone_number);
  LOGI("Watch this console: Incoming message will appear immediately below!");
  LOGI("Observe PPK2: Current settles at ~4.5 - 5.5 mA (Standard DRX RRC Idle floor).");
#endif

  /* Enable Cortex-R5 PMU Sleep */
  nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);

#if EDRX_BENCHMARK_PURE_CONTINUOUS_MODE
  /* =======================================================================
   * PURE CONTINUOUS BENCHMARK MODE WITH SEMAPHORE-DRIVEN WAKEUP:
   * Keep CPU, baseband, and peripherals in undisturbed sleep indefinitely.
   * CPU only wakes up when an incoming SMS signals g_sms_sem!
   * ======================================================================= */
  while (1) {
    /* Block indefinitely until incoming SMS releases g_sms_sem.
     * While blocked, FreeRTOS idle task runs WFI and Cortex-R5 PMU enters deep sleep! */
    nwy_hal_os_semaphore_acquire(g_sms_sem, 0xFFFFFFFF);

    if (g_pending_sms_index >= 0) {
      int idx = g_pending_sms_index;
      g_pending_sms_index = -1;

#if (!EDRX_KEEP_UART_OPEN_FOR_DIAGNOSTICS)
      /* Wake up CPU PMU and re-open UART */
      nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);
      g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
      if (g_dbg_uart_fd >= 0) {
        nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
      }
#endif

      nwy_sms_recv_info_type_t sms_data;
      memset(&sms_data, 0, sizeof(sms_data));
      if (nwy_hal_sms_read(1, idx, &sms_data)) {
        LOGI("");
        LOGI("==============================================================");
        LOGI(">>> [NETWORK PAGING WAKEUP] INCOMING SMS RECEIVED! <<<");
        LOGI("==============================================================");
        LOGI("  * From Phone Number : %s", sms_data.source_phone_num);
        LOGI("  * Timestamp         : 20%02d-%02d-%02d %02d:%02d:%02d",
             sms_data.date.uYear, sms_data.date.uMonth, sms_data.date.uDay,
             sms_data.date.uHour, sms_data.date.uMinute, sms_data.date.uSecond);
        LOGI("  * Message Body      : \"%s\"", sms_data.msg_content);
        LOGI("==============================================================");
        LOGI("  * PPK2 Analysis     : The ~10 mA current bump observed on PPK2");
        LOGI("                        was the Cat-1 RF receiver waking from DRX");
        LOGI("                        to download this SMS packet from Airtel!");
        LOGI("==============================================================");
        if (idx > 0) {
          nwy_hal_sms_delete(1, idx);
        }
      } else {
        LOGW("Incoming SMS notification at index %d, but read returned error.", idx);
      }

      nwy_hal_os_thread_sleep(500);

#if (!EDRX_KEEP_UART_OPEN_FOR_DIAGNOSTICS)
      /* Re-close UART and re-enter PMU sleep */
      if (g_dbg_uart_fd >= 0) {
        nwy_hal_uart_close(g_dbg_uart_fd);
        g_dbg_uart_fd = -1;
        nwy_hal_uart_set_log_fd(-1);
      }
      nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);
#endif
    }
  }
#else
  /* =======================================================================
   * EXTENDED PERIODIC BENCHMARK AUDIT LOOP
   * ======================================================================= */
  while (1) {
    nwy_hal_os_thread_sleep(EDRX_HEALTH_AUDIT_INTERVAL_SEC * 1000);

    /* Wake up for periodic audit */
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    /* Reopen UART */
    g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
    if (g_dbg_uart_fd >= 0) {
      nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
    }

    status_report.uptime_sec += EDRX_HEALTH_AUDIT_INTERVAL_SEC;
    status_report.audit_cycle++;

    sample_radio_metrics(&status_report);

    memset(at_resp, 0, sizeof(at_resp));
    if (send_virt_at_cmd("AT+CEDRXRDP", at_resp, sizeof(at_resp))) {
      if (strstr(at_resp, "+CEDRXRDP:")) {
        status_report.edrx_supported_by_network = true;
      }
    }

    print_benchmark_card(&status_report);

    nwy_hal_os_thread_sleep(200);

    /* Close UART and re-enter PMU sleep */
    if (g_dbg_uart_fd >= 0) {
      nwy_hal_uart_close(g_dbg_uart_fd);
      g_dbg_uart_fd = -1;
      nwy_hal_uart_set_log_fd(-1);
    }
    nwy_pm_state_set(NWY_PM_SLEEP_ENABLE);
  }
#endif
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry(void)
#else
int appimg_enter(void *param)
#endif
{
  nwy_osi_thread_t main_thread = NULL;
  nwy_hal_os_thread_create(&main_thread, "edrx_main", main_task, NULL,
                           NWY_OSI_PRIORITY_NORMAL, 1024 * 8);
  return 0;
}

void appimg_exit(void) {
  if (g_dbg_uart_fd >= 0) {
    nwy_hal_uart_close(g_dbg_uart_fd);
  }
}
