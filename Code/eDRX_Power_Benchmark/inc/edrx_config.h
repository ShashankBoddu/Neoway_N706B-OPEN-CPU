/**
 * @file edrx_config.h
 * @brief Industrial eDRX (Extended Discontinuous Reception) Configuration & Types
 * @note Target Platform: Neoway N706B (ASR1605S Cat-1)
 */

#ifndef EDRX_CONFIG_H
#define EDRX_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION "1.0.0-EDRX-PROD"
#define TAG "EDRX_BM"

/* =========================================================================
 * SERIAL & LOGGING CONFIGURATION
 * ========================================================================= */
#define DEBUG_UART_PORT "URT1"
#define DEBUG_UART_BAUD 115200

/* When set to 0, UART1 is closed during sleep to gate AP bus clocks (< 1.5 mA).
 * When an incoming SMS arrives, UART1 is automatically re-opened to display the message.
 * When set to 1, UART1 is kept open, which holds the AP clock active (~4 mA extra). */
#define EDRX_KEEP_UART_OPEN_FOR_DIAGNOSTICS 0

/* =========================================================================
 * HARDWARE POWER GATING & CARRIER ISOLATION PINS
 * ========================================================================= */
#define PIN_PERIPHERAL_POWER_GPIO0 0  /* High-side P-MOS switch Q3/Q1 for VCC_SWout */

/* Carrier board pins exposed on headers to pull LOW to prevent leakage */
static const uint32_t s_carrier_isolation_gpios[] = {1, 5, 7, 57, 84, 87};

/* =========================================================================
 * USB POWER MANAGEMENT IN EDRX
 * =========================================================================
 * When set to 1, the USB 2.0 PHY is deactivated during the undisturbed eDRX
 * listening intervals to eliminate the ~10 mA PC host controller current.
 * When set to 0, USB COM ports (COM21/COM27) remain permanently connected.
 */
#define EDRX_BENCHMARK_USB_PHY_SLEEP 1

/* =========================================================================
 * 3GPP REL-13 EDRX PARAMETER DEFINITIONS (E-UTRAN / LTE WB-S1)
 * ========================================================================= */
#define EDRX_ACT_TYPE_EUTRAN 4  /* 4 = E-UTRAN (LTE Cat-1 WB-S1) */

/* 4-bit Requested eDRX value strings */
#define EDRX_VAL_5_12_SEC    "0000"  /* 5.12 seconds */
#define EDRX_VAL_10_24_SEC   "0001"  /* 10.24 seconds */
#define EDRX_VAL_20_48_SEC   "0010"  /* 20.48 seconds */
#define EDRX_VAL_40_96_SEC   "0011"  /* 40.96 seconds */
#define EDRX_VAL_61_44_SEC   "0100"  /* 61.44 seconds */
#define EDRX_VAL_81_92_SEC   "0101"  /* 81.92 seconds */
#define EDRX_VAL_102_4_SEC   "0110"  /* 102.40 seconds */
#define EDRX_VAL_122_88_SEC  "0111"  /* 122.88 seconds */
#define EDRX_VAL_143_36_SEC  "1000"  /* 143.36 seconds */
#define EDRX_VAL_163_84_SEC  "1001"  /* 163.84 seconds */
#define EDRX_VAL_327_68_SEC  "1010"  /* 327.68 seconds */
#define EDRX_VAL_655_36_SEC  "1011"  /* 655.36 seconds */
#define EDRX_VAL_1310_72_SEC "1100"  /* 1310.72 seconds */
#define EDRX_VAL_2621_44_SEC "1101"  /* 2621.44 seconds */

/* Default requested eDRX value: 20.48 seconds (most widely granted Rel-13 cycle by Airtel & Jio) */
#define EDRX_REQUESTED_DEFAULT EDRX_VAL_20_48_SEC

/* Undisturbed eDRX listening interval for clean PPK2 current benchmarking:
 * In continuous mode (1), the module enters pure steady-state listening after the
 * initial diagnostic report, completely eliminating periodic AT/IPC queries and
 * preventing eNodeB RRC Connected (~20 mA) transitions.
 * When set to 0, periodic audits run every EDRX_HEALTH_AUDIT_INTERVAL_SEC seconds. */
#define EDRX_BENCHMARK_PURE_CONTINUOUS_MODE 1
#define EDRX_HEALTH_AUDIT_INTERVAL_SEC      600  /* 10 minutes */

/* =========================================================================
 * INDIAN MNO APN AUTO-DETECTION TABLE
 * ========================================================================= */
typedef struct {
  const char *mcc_mnc_prefix;  /* 5 or 6 digit IMSI prefix */
  const char *operator_name;   /* Human-readable network name */
  const char *primary_apn;     /* Primary APN string */
  const char *fallback_apn;    /* Secondary/M2M APN */
} indian_mno_profile_t;

static const indian_mno_profile_t s_indian_mno_db[] = {
  {"40445", "Airtel India (Karnataka)",        "airtelgprs.com", "airtelm2m.com"},
  {"40449", "Airtel India (AP & Telangana)",   "airtelgprs.com", "airtelm2m.com"},
  {"40410", "Airtel India (Delhi/NCR)",        "airtelgprs.com", "airtelm2m.com"},
  {"40492", "Airtel India (Mumbai)",           "airtelgprs.com", "airtelm2m.com"},
  {"40470", "Airtel India (Rajasthan)",        "airtelgprs.com", "airtelm2m.com"},
  {"40402", "Airtel India (Punjab)",           "airtelgprs.com", "airtelm2m.com"},
  {"40403", "Airtel India (HP)",               "airtelgprs.com", "airtelm2m.com"},
  {"40416", "Airtel India (NE)",               "airtelgprs.com", "airtelm2m.com"},
  {"40431", "Airtel India (Kolkata)",          "airtelgprs.com", "airtelm2m.com"},
  {"405854", "Reliance Jio 4G",                "jionet",         "jionet"},
  {"405855", "Reliance Jio 4G",                "jionet",         "jionet"},
  {"405856", "Reliance Jio 4G",                "jionet",         "jionet"},
  {"405857", "Reliance Jio 4G",                "jionet",         "jionet"},
  {"405858", "Reliance Jio 4G",                "jionet",         "jionet"},
  {"40404", "Vodafone Idea (Delhi)",           "portalnmms",     "www"},
  {"40420", "Vodafone Idea (Mumbai)",          "portalnmms",     "www"},
  {"40446", "Vodafone Idea (Karnataka)",       "portalnmms",     "www"},
  {"40486", "Vodafone Idea (AP & Telangana)",  "portalnmms",     "www"},
  {"40434", "BSNL Mobile",                    "bsnlnet",        "bsnlnet"}
};

/* =========================================================================
 * EDRX RUNTIME TELEMETRY STRUCT
 * ========================================================================= */
typedef struct {
  bool edrx_supported_by_network;
  bool edrx_granted_by_mme;
  int  act_type;
  char requested_tedrx_bits[8];
  char granted_tedrx_bits[8];
  char granted_ptw_bits[8];
  float granted_tedrx_seconds;
  float granted_ptw_seconds;
  char oper_numeric[8];
  char oper_alpha[32];
  char active_apn[32];
  char phone_number[32];
  char imsi[32];
  char iccid[32];
  int  csq_rssi;
  int  csq_ber;
  int  rsrp_dbm;
  int  rsrq_db;
  int  sinr_db;
  int  vbat_mv;
  uint32_t uptime_sec;
  uint32_t audit_cycle;
} edrx_status_report_t;

#ifdef __cplusplus
}
#endif

#endif /* EDRX_CONFIG_H */
