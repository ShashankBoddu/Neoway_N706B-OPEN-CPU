#include <stdlib.h>
#include <string.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_sim_api.h"
#include "nwy_data_api.h"
#include "nwy_usb_serial.h"
#include "nwy_gpio_api.h"
#include <stdio.h>
#include <stdarg.h>

#define GPIO_NET_STATUS 69 // NET LED
#define GPIO_STATUS     70 // STATUS LED

volatile int g_net_status = 0; 
// 0 = Not Registered / Searching
// 1 = Registered (Network OK)
// 2 = Data Call Connected (IP Acquired)

static void led_task(void *param) {
    nwy_gpio_direction_set(GPIO_NET_STATUS, PIN_DIRECTION_OUT);
    nwy_gpio_direction_set(GPIO_STATUS, PIN_DIRECTION_OUT);

    // STATUS LED solid ON to indicate module is running
    nwy_gpio_value_set(GPIO_STATUS, PIN_LEVEL_HIGH);

    int toggle = 0;
    while(1) {
        if (g_net_status == 0) {
            // Searching: Blink fast (200ms)
            toggle = !toggle;
            nwy_gpio_value_set(GPIO_NET_STATUS, toggle ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
            nwy_thread_sleep(200);
        } else if (g_net_status == 1) {
            // Registered: Blink slow (1000ms)
            toggle = !toggle;
            nwy_gpio_value_set(GPIO_NET_STATUS, toggle ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
            nwy_thread_sleep(1000);
        } else if (g_net_status == 2) {
            // Data Connected: Solid ON
            nwy_gpio_value_set(GPIO_NET_STATUS, PIN_LEVEL_HIGH);
            nwy_thread_sleep(1000);
        }
    }
}

static void serial_log(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsprintf(buf, fmt, args);
    va_end(args);
    if (len > 0) {
        nwy_usb_serial_send(buf, len);
        nwy_usb_serial_send("\r\n", 2);
    }
}


static const char* get_reg_state_str(nwy_nw_service_type_e state) {
    switch(state) {
        case NWY_NW_SERVICE_NONE: return "OOS (No Service)";
        case NWY_NW_SERVICE_LIMITED: return "Limited Service";
        case NWY_NW_SERVICE_FULL: return "Full Service";
        default: return "Unknown";
    }
}

static void network_monitor_data_cb(int profile_idx, nwy_data_call_state_e ind_state) {
    if (ind_state == NWY_DATA_CALL_CONNECTED_STATE) {
        serial_log("DATA CALLBACK: Connected!");
    } else {
        serial_log("DATA CALLBACK: Disconnected!");
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
    
    serial_log("--- Network Monitor Started ---");
    
    while(1) {
        // 1. Check SIM Status
        g_net_status = 0; // Default to not ready each loop until proven otherwise
        if (nwy_sim_status_get(sim_id, &sim_status) == NWY_SUCCESS) {
            serial_log("SIM Status: %s", (sim_status == NWY_SIM_READY) ? "READY" : "NOT READY");
        }
        
        if (sim_status == NWY_SIM_READY) {
            // 2. Check Registration Status
            memset(&reg_status, 0, sizeof(reg_status));
            if (nwy_nw_regstatus_get(sim_id, &reg_status) == NWY_SUCCESS) {
                if (reg_status.cs_regs_valid) {
                    serial_log("CS Reg: %s, RAT: %s", 
                        get_reg_state_str(reg_status.cs_regs.regs_state),
                        get_rat_str(reg_status.cs_regs.rat_type));
                }
                if (reg_status.ps_regs_valid) {
                    serial_log("PS Reg: %s, RAT: %s", 
                        get_reg_state_str(reg_status.ps_regs.regs_state),
                        get_rat_str(reg_status.ps_regs.rat_type));
                        
                    // If PS Registration is FULL, check and start Data Call
                    if (reg_status.ps_regs.regs_state == NWY_NW_SERVICE_FULL) {
                        g_net_status = 1; // At least registered
                        
                        nwy_data_callinfo_t call_info;
                        memset(&call_info, 0, sizeof(call_info));
                        
                        if (nwy_data_call_info_get(1, &call_info) == NWY_SUCCESS) {
                            if (call_info.state == NWY_DATA_CALL_CONNECTED_STATE) {
                                serial_log("Data Call Connected! IP: %s", call_info.ipv4_str);
                                g_net_status = 2; // Data connected
                            } else {
                                serial_log("Data Call Not Connected. Starting Call...");
                                nwy_data_start_call_t call_param;
                                memset(&call_param, 0, sizeof(call_param));
                                call_param.cid = 1;
                                call_param.action = NWY_DATA_CALL_ACT;
                                call_param.trigger_type = NWY_DATA_TRIGGER_OPEN; 
                                call_param.call_auto_type = NWY_DATA_CALL_AUTO_TYPE_ENABLE;
                                call_param.set_profile.pdp_type = -1; // NWY_DATA_PDP_TYPE_MIN
                                call_param.set_profile.auth_proto = -1; // NWY_DATA_AUTH_PROTO_MIN
                                call_param.if_internal_call = 1; 

                                // CRITICAL: Register the callback. If not registered, the modem jumps to NULL and crashes!
                                nwy_data_reg_cb(sim_id, 1, network_monitor_data_cb);
                                nwy_data_call_start(sim_id, &call_param);
                            }
                        }
                    }
                }
            }
            
            // 3. Check Signal Strength
            memset(&csq_info, 0, sizeof(csq_info));
            if (nwy_nw_csq_get(sim_id, &csq_info) == NWY_SUCCESS) {
                serial_log("Signal (CSQ): %d, BER: %d", csq_info.csq_rssi_level, csq_info.ber);
            }
            
            // 4. Check Operator Info
            memset(&opt_info, 0, sizeof(opt_info));
            if (nwy_nw_operator_get(sim_id, &opt_info) == NWY_SUCCESS) {
                serial_log("Operator: %s (%s%s)", opt_info.long_eons, opt_info.mcc, opt_info.mnc);
            }
        } else {
            serial_log("Waiting for SIM tray / Network...");
        }
        
        serial_log("-------------------------------");
        nwy_thread_sleep(10000); // Check every 10 seconds
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
    nwy_thread_sleep(10 * 1000); // wait for PC to enumerate USB serial
    serial_log("Network Monitor App Entered");
    
    nwy_osi_thread_t net_thread = NULL;
    nwy_error_e ret = nwy_thread_create(&net_thread, "net_monitor", NWY_OSI_PRIORITY_NORMAL, network_monitor_task, NULL, 10, 1024 * 4, NULL);
    if (ret != NWY_SUCCESS) {
        serial_log("Failed to create network thread! Error: %d", ret);
    } else {
        serial_log("Network thread created successfully.");
    }
    
    nwy_osi_thread_t led_th_hdl = NULL;
    nwy_thread_create(&led_th_hdl, "led_task", NWY_OSI_PRIORITY_NORMAL, led_task, NULL, 10, 1024 * 2, NULL);
    
    return 0;
}

void appimg_exit(void)
{
    serial_log("Network Monitor App Exited");
}
