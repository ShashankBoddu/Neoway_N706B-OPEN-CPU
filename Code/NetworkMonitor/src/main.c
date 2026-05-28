#include <stdlib.h>
#include <string.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_sim_api.h"

static const char* get_reg_state_str(nwy_nw_service_type_e state) {
    switch(state) {
        case NWY_NW_SERVICE_NONE: return "OOS (No Service)";
        case NWY_NW_SERVICE_LIMITED: return "Limited Service";
        case NWY_NW_SERVICE_FULL: return "Full Service";
        default: return "Unknown";
    }
}

static const char* get_rat_str(nwy_nw_rat_type_e rat) {
    switch(rat) {
        case NWY_NW_RAT_GSM: return "GSM (2G)";
        case NWY_NW_RAT_WCDMA: return "WCDMA (3G)";
        case NWY_NW_RAT_LTE: return "LTE (4G)";
        case NWY_NW_RAT_NBIoT: return "NB-IoT";
        case NWY_NW_RAT_NONE: return "None";
        default: return "Other";
    }
}

static void network_monitor_task(void *param)
{
    nwy_sim_id_e sim_id = NWY_SIM_ID_SLOT_1;
    nwy_sim_status_e sim_status;
    nwy_nw_regstatus_t reg_status;
    nwy_nw_get_csq_info_t csq_info;
    nwy_nw_operator_t opt_info;
    
    nwy_thread_sleep(5000); // Wait for module to initialize
    
    NWY_SDK_LOG_DEBUG("--- Network Monitor Started ---");
    
    while(1) {
        // 1. Check SIM Status
        if (nwy_sim_status_get(sim_id, &sim_status) == NWY_SUCCESS) {
            NWY_SDK_LOG_DEBUG("SIM Status: %s", (sim_status == NWY_SIM_READY) ? "READY" : "NOT READY");
        }
        
        if (sim_status == NWY_SIM_READY) {
            // 2. Check Registration Status
            memset(&reg_status, 0, sizeof(reg_status));
            if (nwy_nw_regstatus_get(sim_id, &reg_status) == NWY_SUCCESS) {
                if (reg_status.cs_regs_valid) {
                    NWY_SDK_LOG_DEBUG("CS Reg: %s, RAT: %s", 
                        get_reg_state_str(reg_status.cs_regs.regs_state),
                        get_rat_str(reg_status.cs_regs.rat_type));
                }
                if (reg_status.ps_regs_valid) {
                    NWY_SDK_LOG_DEBUG("PS Reg: %s, RAT: %s", 
                        get_reg_state_str(reg_status.ps_regs.regs_state),
                        get_rat_str(reg_status.ps_regs.rat_type));
                }
            }
            
            // 3. Check Signal Strength
            memset(&csq_info, 0, sizeof(csq_info));
            if (nwy_nw_csq_get(sim_id, &csq_info) == NWY_SUCCESS) {
                NWY_SDK_LOG_DEBUG("Signal (CSQ): %d, BER: %d", csq_info.csq_rssi_level, csq_info.ber);
            }
            
            // 4. Check Operator Info
            memset(&opt_info, 0, sizeof(opt_info));
            if (nwy_nw_operator_get(sim_id, &opt_info) == NWY_SUCCESS) {
                NWY_SDK_LOG_DEBUG("Operator: %s (%s%s)", opt_info.long_eons, opt_info.mcc, opt_info.mnc);
            }
        } else {
            NWY_SDK_LOG_DEBUG("Waiting for SIM tray / Network...");
        }
        
        NWY_SDK_LOG_DEBUG("-------------------------------");
        nwy_thread_sleep(10000); // Check every 10 seconds
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
    nwy_thread_sleep(1000);
    NWY_SDK_LOG_DEBUG("Network Monitor App Entered");
    
    nwy_osi_thread_t net_thread = NULL;
    nwy_thread_create(&net_thread, "net_monitor", NWY_OSI_PRIORITY_NORMAL, network_monitor_task, NULL, 0, 1024 * 4, NULL);
    
    return 0;
}

void appimg_exit(void)
{
    NWY_SDK_LOG_DEBUG("Network Monitor App Exited");
}
