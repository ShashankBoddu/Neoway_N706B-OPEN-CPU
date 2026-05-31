#include "hal/uart/nwy_hal_uart.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/net/nwy_hal_net.h"
#include "hal/pm/nwy_hal_pm.h"
#include "hal/os/nwy_hal_os.h"
#include "nwy_log_api.h"
#include "nwy_osi_api.h"
#include "nwy_network_api.h"
#include "nwy_data_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

#define TAG "NetMonitor"
#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;
// Redefine printf to map directly to the opened UART port
#define printf(fmt, ...) nwy_hal_uart_printf(g_uart_fd, fmt, ##__VA_ARGS__)

static const char *get_reg_state_str(int state) {
  switch (state) {
    case NWY_NW_SERVICE_NONE:    return "OUT OF SERVICE (0)";
    case NWY_NW_SERVICE_LIMITED: return "LIMITED SERVICE (1)";
    case NWY_NW_SERVICE_FULL:    return "FULL SERVICE (2)";
    default:                     return "UNKNOWN";
  }
}

static const char *get_rat_str(int rat) {
  switch (rat) {
    case NWY_NW_RAT_NONE:     return "NONE";
    case NWY_NW_RAT_GSM:      return "GSM (2G)";
    case NWY_NW_RAT_WCDMA:    return "WCDMA (3G)";
    case NWY_NW_RAT_LTE:      return "LTE (4G)";
    case NWY_NW_RAT_CATM:     return "CAT-M";
    case NWY_NW_RAT_NBIoT:    return "NB-IoT";
    case NWY_NW_RAT_NR:       return "5G NR";
    case NWY_NW_RAT_CDMA:     return "CDMA";
    case NWY_NW_RAT_HDR:      return "HDR";
    case NWY_NW_RAT_TDSCDMA:  return "TD-SCDMA";
    default:                  return "UNKNOWN";
  }
}

static void get_net_mode_str(int mode, char *buf, int max_len) {
  if (mode == NWY_NW_MODE_MASK_AUTO) {
    snprintf(buf, max_len, "AUTO (unlocked)");
    return;
  }
  buf[0] = '\0';
  int len = 0;
  if (mode & NWY_NW_MODE_MASK_GSM) {
    len += snprintf(buf + len, max_len - len, "GSM ");
  }
  if (mode & NWY_NW_MODE_MASK_WCDMA) {
    len += snprintf(buf + len, max_len - len, "WCDMA ");
  }
  if (mode & NWY_NW_MODE_MASK_LTE) {
    len += snprintf(buf + len, max_len - len, "LTE ");
  }
  if (mode & NWY_NW_MODE_MASK_SA) {
    len += snprintf(buf + len, max_len - len, "SA ");
  }
  if (mode & NWY_NW_MODE_MASK_CATM) {
    len += snprintf(buf + len, max_len - len, "CATM ");
  }
  if (mode & NWY_NW_MODE_MASK_NB) {
    len += snprintf(buf + len, max_len - len, "NB ");
  }
  if (len == 0) {
    snprintf(buf, max_len, "OTHER (0x%X)", mode);
  } else {
    if (len > 0 && buf[len - 1] == ' ') {
      buf[len - 1] = '\0';
    }
  }
}

// Convert CSQ level (0-31) to approximate dBm
static int csq_to_dbm(int csq) {
  if (csq == 99) return 0;
  return -113 + (csq * 2);
}

// Callback for SIM Hotplug/Status changes
static void test_sim_urc_cb(int sim_id, bool card_present, nwy_sim_status_e sim_status) {
  LOGI("SIM EVENT - SIM %d: Present=%s, Status=%d", 
       sim_id, card_present ? "YES" : "NO", (int)sim_status);
}

// Callback for GPRS data call status changes
static void test_data_call_cb(int sim_id, int profile_idx, bool connected, const char *ip_address) {
  if (connected) {
    LOGI("DATA CALL CONNECTED - SIM %d, Profile %d, IP: %s", 
         sim_id, profile_idx, ip_address ? ip_address : "N/A");
  } else {
    LOGW("DATA CALL DISCONNECTED - SIM %d, Profile %d", sim_id, profile_idx);
  }
}

// Run the network diagnostics self-test
static void run_network_diagnostics_scan(void) {
  printf("\r\n");
  printf("==================================================\r\n");
  printf("         NETWORK HAL DIAGNOSTIC SELF-TEST REPORT  \r\n");
  printf("==================================================\r\n");

  int target_sim = 1; // Default Slot 1
  bool radio_restart_needed = false;

  // 1. Check SIM Status first
  bool sim_ready = nwy_hal_sim_is_ready(target_sim);
  LOGI("SIM Slot %d Status: %s", target_sim, sim_ready ? "READY" : "NOT READY");
  if (!sim_ready) {
    LOGW("SIM is not ready. Network registration will fail or remain offline.");
  }

  // 2. Query Radio State
  nwy_nw_radio_mode_e radio_mode = NWY_NW_RADIO_MIN_MODE;
  if (nwy_hal_net_get_radio_mode(&radio_mode)) {
    LOGI("Radio State: %s (%d)", 
         radio_mode == NWY_NW_RADIO_MIN_MODE ? "POWER OFF (0)" : 
         (radio_mode == NWY_NW_RADIO_NORMAL_MODE ? "POWER ON (1)" : 
          (radio_mode == NWY_NW_RADIO_FLIGHT_MODE ? "FLIGHT MODE (4)" : "UNKNOWN")), (int)radio_mode);
  } else {
    LOGE("Radio State: Failed to query");
  }

  // 3. Query Network Mode settings & Lock to LTE for reliability
  int net_mode = 0;
  if (nwy_hal_net_get_mode(target_sim, &net_mode)) {
    char net_mode_buf[64];
    get_net_mode_str(net_mode, net_mode_buf, sizeof(net_mode_buf));
    LOGI("Preferred Network RAT Mode: %s (0x%X)", net_mode_buf, net_mode);
    
    // Lock to LTE (0x10) to match the known working configuration for 4G carriers like Jio
    if (net_mode != NWY_NW_MODE_MASK_LTE) {
      LOGW("Network Mode is locked to 0x%X. Locking to LTE (0x10) for faster 4G registration...", net_mode);
      if (nwy_hal_net_set_mode(target_sim, NWY_NW_MODE_MASK_LTE)) {
        LOGI("Network Mode locked to LTE successfully.");
        radio_restart_needed = true;
      } else {
        LOGE("Failed to lock Network Mode to LTE.");
      }
    }
  } else {
    LOGE("Preferred Network RAT Mode: Failed to query");
  }

  // 4. Query VoLTE IMS setting & Enable if disabled
  uint8_t ims_mode = 0xFF;
  if (nwy_hal_net_get_ims_mode(target_sim, &ims_mode)) {
    LOGI("VoLTE / IMS Mode: %s (%d)", 
         ims_mode == 0 ? "DISABLED" : (ims_mode == 1 ? "ENABLED" : "AUTO"), (int)ims_mode);
    if (ims_mode == 0) {
      LOGW("VoLTE is disabled. Enabling VoLTE/IMS (critical for LTE-only carriers like Jio)...");
      if (nwy_hal_net_set_ims_mode(target_sim, 1)) {
        LOGI("VoLTE / IMS enabled successfully.");
        radio_restart_needed = true;
      } else {
        LOGE("Failed to enable VoLTE / IMS.");
      }
    }
  } else {
    LOGE("VoLTE / IMS Mode: Failed to query");
  }

  // 4.5. Query custom configs (NetAuto, UE Mode, Data-Only)
  nwy_nw_config_info_u cfg_info;
  memset(&cfg_info, 0, sizeof(cfg_info));
  if (nwy_hal_net_get_custom_cfg(target_sim, NWY_NW_CONFIG_RW_NETAUTO, &cfg_info)) {
    LOGI("NetAuto Config: %s, scan timer: %d min", 
         cfg_info.netauto.onoff == 1 ? "ENABLED" : "DISABLED", cfg_info.netauto.timer);
    if (cfg_info.netauto.onoff == 0) {
      LOGW("NetAuto is disabled. Attempting to enable NetAuto...");
      cfg_info.netauto.onoff = 1;
      cfg_info.netauto.timer = 3;
      int ret = nwy_nw_config_set(nwy_hal_get_sim_id_enum(target_sim), NWY_NW_CONFIG_RW_NETAUTO, &cfg_info);
      if (ret == 0) {
        LOGI("NetAuto enabled successfully.");
      } else if (ret == NWY_GEN_E_PLAT_NOT_SUPPORT) {
        LOGI("NetAuto configuration is not supported on this platform (ASR1605). Skipping.");
      } else {
        LOGE("Failed to enable NetAuto, error=%d.", ret);
      }
    }
  } else {
    LOGI("NetAuto Config: Not supported or query failed");
  }

  memset(&cfg_info, 0, sizeof(cfg_info));
  if (nwy_hal_net_get_custom_cfg(target_sim, NWY_NW_CONFIG_RW_UEMODE, &cfg_info)) {
    LOGI("UE Mode Configuration: %s (%d)", 
         cfg_info.uemode == NWY_NW_PS_MODE_II ? "PS ONLY (Data Centric)" :
         (cfg_info.uemode == NWY_NW_CS_PS_MODE_I ? "CS + PS (Voice Centric)" :
          (cfg_info.uemode == NWY_NW_CS_PS_MODI_II ? "CS + PS (Data Centric)" :
           (cfg_info.uemode == NWY_NW_PS_MODE_I ? "PS ONLY (Voice Centric)" : "UNKNOWN"))),
         (int)cfg_info.uemode);
  } else {
    LOGE("UE Mode Configuration: Failed to query");
  }

  memset(&cfg_info, 0, sizeof(cfg_info));
  if (nwy_hal_net_get_custom_cfg(target_sim, NWY_NW_CONFIG_RW_DATAONLY, &cfg_info)) {
    LOGI("Data-Only Configuration: %s (%d)", 
         cfg_info.dataonly == NWY_NW_CFG_ENABLE ? "ENABLED" : "DISABLED",
         (int)cfg_info.dataonly);
  } else {
    LOGE("Data-Only Configuration: Failed to query");
  }

  // 5. Query PSM settings
  nwy_nw_psm_info_t psm_info;
  memset(&psm_info, 0, sizeof(psm_info));
  if (nwy_hal_net_get_psm_info(target_sim, &psm_info)) {
    LOGI("Power Saving Mode (PSM): %s", psm_info.mode == NWY_NW_PSM_MODE_DISABLE ? "DISABLED" : "ENABLED");
  } else {
    LOGE("PSM Settings: Failed to query");
  }

  // 6. Query Registration Status
  int cs_state = 0, ps_state = 0, rat = 0;
  if (nwy_hal_net_get_registration_details(target_sim, &cs_state, &ps_state, &rat)) {
    LOGI("CS Domain Registration: %s", get_reg_state_str(cs_state));
    LOGI("PS Domain Registration: %s", get_reg_state_str(ps_state));
    LOGI("Active Radio Technology: %s", get_rat_str(rat));
  } else {
    LOGE("Registration Status: Failed to query");
  }

  // 7. Query Operator & Carrier Info
  char op_name[64] = "N/A";
  if (nwy_hal_net_get_operator_name(target_sim, op_name, sizeof(op_name))) {
    LOGI("Registered Operator: %s", op_name);
  } else {
    LOGW("Registered Operator: Out of service / Not registered");
  }

  nwy_nw_operator_t *op_details = (nwy_nw_operator_t *)malloc(sizeof(nwy_nw_operator_t));
  if (op_details) {
    memset(op_details, 0, sizeof(nwy_nw_operator_t));
    if (nwy_hal_net_get_operator_details(target_sim, op_details)) {
      LOGI("  Operator EONS (Long):  %s", op_details->long_eons);
      LOGI("  Operator EONS (Short): %s", op_details->short_eons);
      LOGI("  Operator PLMN MCC/MNC: %s-%s", op_details->mcc, op_details->mnc);
      LOGI("  Carrier SPN Name:      %s", op_details->spn);
    }
    free(op_details);
  }

  // 8. Query Signal Strength & Bit Error Rate
  int csq = 99, ber = 99;
  if (nwy_hal_net_get_csq(target_sim, &csq, &ber)) {
    LOGI("Signal Strength (CSQ): %d (%d dBm), BER=%d", csq, csq_to_dbm(csq), ber);
  } else {
    LOGE("Signal Strength (CSQ): Failed to query");
  }

  // 9. Query Advanced Signal Info
  nwy_nw_signal_info_t *sig_info = (nwy_nw_signal_info_t *)malloc(sizeof(nwy_nw_signal_info_t));
  if (sig_info) {
    memset(sig_info, 0, sizeof(nwy_nw_signal_info_t));
    if (nwy_hal_net_get_signal_info(target_sim, sig_info)) {
      LOGI("Advanced RF Signal Parameters (RAT=%d):", sig_info->rat);
      if (sig_info->rat == NWY_NW_RAT_LTE) {
        LOGI("  LTE RSRP: %d (%d.%d dBm)", 
             sig_info->rat_signal_info.lte_signal_info.rsrp, 
             sig_info->rat_signal_info.lte_signal_info.rsrp / 10,
             abs(sig_info->rat_signal_info.lte_signal_info.rsrp % 10));
        LOGI("  LTE RSRQ: %d (%d.%d dB)", 
             sig_info->rat_signal_info.lte_signal_info.rsrq, 
             sig_info->rat_signal_info.lte_signal_info.rsrq / 10,
             abs(sig_info->rat_signal_info.lte_signal_info.rsrq % 10));
        LOGI("  LTE RSSI: %d (%d.%d dBm)", 
             sig_info->rat_signal_info.lte_signal_info.rssi, 
             sig_info->rat_signal_info.lte_signal_info.rssi / 10,
             abs(sig_info->rat_signal_info.lte_signal_info.rssi % 10));
        LOGI("  LTE SINR: %d (%d.%d dB)", 
             sig_info->rat_signal_info.lte_signal_info.sinr, 
             sig_info->rat_signal_info.lte_signal_info.sinr / 10,
             abs(sig_info->rat_signal_info.lte_signal_info.sinr % 10));
      } else if (sig_info->rat == NWY_NW_RAT_GSM) {
        LOGI("  GSM RSSI: %d dBm", sig_info->rat_signal_info.gsm_signal_info.rssi);
        LOGI("  GSM BER:  %d", sig_info->rat_signal_info.gsm_signal_info.ber);
      } else if (sig_info->rat == NWY_NW_RAT_WCDMA) {
        LOGI("  WCDMA RSCP: %d dBm", sig_info->rat_signal_info.wcdma_signal_info.rscp);
        LOGI("  WCDMA RSSI: %d dBm", sig_info->rat_signal_info.wcdma_signal_info.rssi);
      } else {
        LOGI("  No advanced signal info for current active RAT");
      }
    } else {
      LOGE("Advanced RF Signal Parameters: Failed to query");
    }
    free(sig_info);
  } else {
    LOGE("Advanced RF Signal Parameters: Failed to allocate memory");
  }

  // 10. Query Cell Information
  nwy_nw_cellinfo_t *cell_info = (nwy_nw_cellinfo_t *)malloc(sizeof(nwy_nw_cellinfo_t));
  if (cell_info) {
    memset(cell_info, 0, sizeof(nwy_nw_cellinfo_t));
    if (nwy_hal_net_get_cellinfo(target_sim, NWY_NW_GET_SCELL, cell_info)) {
      LOGI("Serving Cell Information (Type=%d):", cell_info->sub_rat);
      if (cell_info->sub_rat == NWY_RAT_SUB_TDD_LTE || cell_info->sub_rat == NWY_RAT_SUB_FDD_LTE) {
        LOGI("  LTE LAC/TAC:       %d", cell_info->cell_info.lte_cell.serv_cell.tac);
        LOGI("  LTE Cell ID:       %d", cell_info->cell_info.lte_cell.serv_cell.cell_id);
        LOGI("  LTE EARFCN:        %d", cell_info->cell_info.lte_cell.serv_cell.earfcn);
        LOGI("  LTE PCI (Phy ID):  %d", cell_info->cell_info.lte_cell.serv_cell.pci);
        LOGI("  LTE Band:          %d", cell_info->cell_info.lte_cell.serv_cell.band);
        LOGI("  LTE Bandwidth:     %d MHz", 
             cell_info->cell_info.lte_cell.serv_cell.dlBandwidth == 1 ? 1 : 
             (cell_info->cell_info.lte_cell.serv_cell.dlBandwidth == 3 ? 5 : 
             (cell_info->cell_info.lte_cell.serv_cell.dlBandwidth == 4 ? 10 : 
             (cell_info->cell_info.lte_cell.serv_cell.dlBandwidth == 5 ? 15 : 
             (cell_info->cell_info.lte_cell.serv_cell.dlBandwidth == 6 ? 20 : 0)))));
        LOGI("  LTE Duplex Mode:   %s", cell_info->cell_info.lte_cell.serv_cell.isTdd ? "TDD" : "FDD");
      } else if (cell_info->sub_rat == NWY_RAT_SUB_GSM || cell_info->sub_rat == NWY_RAT_SUB_GPRS || cell_info->sub_rat == NWY_RAT_SUB_EDGE) {
        LOGI("  GSM LAC:           %d", cell_info->cell_info.gsm_cell.serv_cell.lac);
        LOGI("  GSM Cell ID:       %d", cell_info->cell_info.gsm_cell.serv_cell.cell_id);
        LOGI("  GSM ARFCN:         %d", cell_info->cell_info.gsm_cell.serv_cell.arfcn);
        LOGI("  GSM BSIC:          %d", cell_info->cell_info.gsm_cell.serv_cell.bsic);
      } else {
        LOGI("  Serving cell data format not decoded for this RAT type");
      }
    } else {
      LOGE("Serving Cell Information: Failed to query");
    }
    free(cell_info);
  } else {
    LOGE("Serving Cell Information: Failed to allocate memory");
  }

  printf("==================================================\r\n");

  if (radio_restart_needed) {
    LOGW("Configuration updated. Performing instant radio power cycle (flight mode toggle) to apply settings...");
    nwy_hal_net_set_radio_mode(NWY_NW_RADIO_FLIGHT_MODE);
    nwy_hal_os_thread_sleep(2000);
    nwy_hal_net_set_radio_mode(NWY_NW_RADIO_NORMAL_MODE);
    LOGI("Radio power cycle completed.");
  }
}

// Background Network Monitor Thread loop
static void network_monitor_task(void *param) {
  // Allow time for startup logs and stable power initialization
  nwy_hal_os_thread_sleep(3000);

  NWY_SDK_LOG_DEBUG("NetworkMonitor: Initializing Debug UART port...");
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd < 0) {
    NWY_SDK_LOG_DEBUG("NetworkMonitor: Failed to open UART %s, error %d", TEST_UART_PORT, g_uart_fd);
    return;
  }

  NWY_SDK_LOG_DEBUG("NetworkMonitor: UART %s opened (fd=%d)", TEST_UART_PORT, g_uart_fd);
  nwy_hal_uart_set_log_fd(g_uart_fd);

  // Print startup welcome menu
  printf("\r\n==================================================\r\n");
  printf("   NEOWAY N706B OPENCPU - NETWORK MONITOR TEST    \r\n");
  printf("==================================================\r\n");
  printf("Port: UART4 (Debug) | Baudrate: 115200\r\n");

  // Enable SIM hotplug hardware detection (critical for slot activity detection)
  nwy_hal_sim_set_detect(1, 1, 0);

  // Register SIM event handler
  nwy_hal_sim_register_urc_cb(1, test_sim_urc_cb);

  // Execute the automated self-test on startup
  run_network_diagnostics_scan();

  bool data_call_dialed = false;
  int oos_counter = 0;

  while (1) {
    nwy_hal_os_thread_sleep(10000); // Check status every 10 seconds

    int cs_state = 0, ps_state = 0, rat = 0;
    nwy_hal_net_get_registration_details(1, &cs_state, &ps_state, &rat);

    int csq = 99, ber = 99;
    nwy_hal_net_get_csq(1, &csq, &ber);

    char ip_address[32] = "N/A";
    bool ip_ok = nwy_hal_net_get_ip(1, 1, ip_address, sizeof(ip_address));

    LOGI("Heartbeat - Reg: CS=%d, PS=%d | Signal: %d CSQ (%d dBm) | IP: %s | RAT: %s",
         cs_state, ps_state, csq, csq_to_dbm(csq), ip_ok ? ip_address : "N/A", get_rat_str(rat));

    // If out of service, increment oos counter. Reset when registered.
    if (cs_state == NWY_NW_SERVICE_NONE && ps_state == NWY_NW_SERVICE_NONE) {
      oos_counter++;
      if (oos_counter >= 6) { // 60 seconds of consecutive Out of Service
        LOGW("Modem Out of Service for 60 seconds. Toggling radio state (flight mode -> normal) to force fresh search...");
        nwy_hal_net_set_radio_mode(NWY_NW_RADIO_FLIGHT_MODE);
        nwy_hal_os_thread_sleep(2000);
        nwy_hal_net_set_radio_mode(NWY_NW_RADIO_NORMAL_MODE);
        oos_counter = 0;
      }
    } else {
      oos_counter = 0;
    }

    // If registered on PS (Data) network but no GPRS call is connected, start data call dial!
    if ((ps_state == NWY_NW_SERVICE_FULL || ps_state == NWY_NW_SERVICE_LIMITED) && !ip_ok) {
      if (!data_call_dialed) {
        LOGI("Network registered. Launching GPRS PDP Context Activation...");
        bool dial_ok = nwy_hal_net_start_data_call(1, 1, test_data_call_cb);
        LOGI("GPRS Activation request sent: %s", dial_ok ? "SUCCESS" : "FAILED");
        data_call_dialed = true;
      }
    } else if (ps_state == NWY_NW_SERVICE_NONE) {
      data_call_dialed = false; // Reset dialing flag if we drop off network
    }
  }
}

// Entry Point
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_thread_sleep(1000);
  NWY_SDK_LOG_DEBUG("NetworkMonitor: Application Entry");

  nwy_osi_thread_t monitor_thread = NULL;
  bool ok = nwy_hal_os_thread_create(&monitor_thread, "net_monitor",
                                     network_monitor_task, NULL,
                                     NWY_OSI_PRIORITY_NORMAL, 1024 * 8);
  NWY_SDK_LOG_DEBUG("NetworkMonitor: Thread spawned, status = %d", ok);

  return 0;
}

// Exit Point
void appimg_exit(void) {
  NWY_SDK_LOG_DEBUG("NetworkMonitor: Application Exit");
  if (g_uart_fd >= 0) {
    nwy_hal_sim_unregister_urc_cb(1);
    nwy_hal_net_stop_data_call(1, 1);
    nwy_hal_uart_close(g_uart_fd);
  }
}
