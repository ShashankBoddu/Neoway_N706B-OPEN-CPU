#include "hal/net/nwy_hal_net.h"
#include "nwy_network_api.h"
#include "nwy_data_api.h"
#include <string.h>

static nwy_hal_net_callback_t g_user_net_cb = NULL;

static void internal_data_cb(int profile_idx, nwy_data_call_state_e ind_state) {
    if (g_user_net_cb) {
        if (ind_state == NWY_DATA_CALL_CONNECTED_STATE) {
            // Retrieve IP to pass to user
            nwy_data_callinfo_t call_info;
            memset(&call_info, 0, sizeof(call_info));
            nwy_data_call_info_get(1, &call_info);
            g_user_net_cb(true, call_info.ipv4_str);
        } else {
            g_user_net_cb(false, NULL);
        }
    }
}

static nwy_sim_id_e get_nwy_sim_id(int sim_id) {
    if (sim_id == 2) {
        return NWY_SIM_ID_SLOT_2;
    }
    return NWY_SIM_ID_SLOT_1;
}

bool nwy_hal_net_is_registered(int sim_id) {
    nwy_nw_regstatus_t reg_status;
    memset(&reg_status, 0, sizeof(reg_status));
    
    if (nwy_nw_regstatus_get(get_nwy_sim_id(sim_id), &reg_status) == 0) {
        if ((reg_status.cs_regs_valid && reg_status.cs_regs.regs_state == NWY_NW_SERVICE_FULL) ||
            (reg_status.ps_regs_valid && reg_status.ps_regs.regs_state == NWY_NW_SERVICE_FULL)) {
            return true;
        }
    }
    return false;
}

bool nwy_hal_net_get_signal(int sim_id, int *rssi) {
    if (!rssi) return false;
    nwy_nw_get_csq_info_t sig;
    if (nwy_nw_csq_get(get_nwy_sim_id(sim_id), &sig) == 0) {
        *rssi = sig.csq_rssi_level;
        return true;
    }
    return false;
}

bool nwy_hal_net_get_csq(int sim_id, int *csq, int *ber) {
    if (!csq || !ber) return false;
    nwy_nw_get_csq_info_t sig;
    if (nwy_nw_csq_get(get_nwy_sim_id(sim_id), &sig) == 0) {
        *csq = sig.csq_rssi_level;
        *ber = sig.ber;
        return true;
    }
    return false;
}

bool nwy_hal_net_start_data_call(int sim_id, nwy_hal_net_callback_t cb) {
    g_user_net_cb = cb;
    
    // Register the callback to prevent modem crash
    nwy_data_reg_cb(get_nwy_sim_id(sim_id), 1, internal_data_cb);
    
    nwy_data_start_call_t call_param;
    memset(&call_param, 0, sizeof(call_param));
    call_param.cid = 1; // Default Profile ID 1
    call_param.action = NWY_DATA_CALL_ACT; // Start Call
    call_param.trigger_type = NWY_DATA_TRIGGER_OPEN;
    call_param.call_auto_type = NWY_DATA_CALL_AUTO_TYPE_ENABLE;
    call_param.set_profile.pdp_type = -1; // Auto Select
    call_param.set_profile.auth_proto = -1; // Auto Select
    call_param.if_internal_call = 1;
    
    return (nwy_data_call_start(get_nwy_sim_id(sim_id), &call_param) == 0);
}

void nwy_hal_net_stop_data_call(int sim_id) {
    nwy_data_start_call_t call_param;
    memset(&call_param, 0, sizeof(call_param));
    call_param.cid = 1;
    call_param.action = NWY_DATA_CALL_DEACT; // Stop Call
    call_param.trigger_type = NWY_DATA_TRIGGER_OPEN;
    call_param.if_internal_call = 1;
    
    nwy_data_call_start(get_nwy_sim_id(sim_id), &call_param);
}

bool nwy_hal_net_get_operator_name(int sim_id, char *name_out, int max_len) {
    if (!name_out || max_len <= 0) return false;
    nwy_nw_operator_t opt;
    memset(&opt, 0, sizeof(opt));
    if (nwy_nw_operator_get(get_nwy_sim_id(sim_id), &opt) == 0) {
        if (strlen(opt.long_eons) > 0) {
            snprintf(name_out, max_len, "%s (%s%s)", opt.long_eons, opt.mcc, opt.mnc);
        } else if (strlen(opt.short_eons) > 0) {
            snprintf(name_out, max_len, "%s (%s%s)", opt.short_eons, opt.mcc, opt.mnc);
        } else {
            snprintf(name_out, max_len, "%s%s", opt.mcc, opt.mnc);
        }
        return true;
    }
    return false;
}

bool nwy_hal_net_get_registration_details(int sim_id, int *cs_state, int *ps_state, int *rat) {
    nwy_nw_regstatus_t reg_status;
    memset(&reg_status, 0, sizeof(reg_status));
    
    if (nwy_nw_regstatus_get(get_nwy_sim_id(sim_id), &reg_status) == 0) {
        if (cs_state) *cs_state = reg_status.cs_regs_valid ? reg_status.cs_regs.regs_state : NWY_NW_SERVICE_NONE;
        if (ps_state) *ps_state = reg_status.ps_regs_valid ? reg_status.ps_regs.regs_state : NWY_NW_SERVICE_NONE;
        if (rat) {
            if (reg_status.ps_regs_valid && reg_status.ps_regs.regs_state == NWY_NW_SERVICE_FULL) {
                *rat = reg_status.ps_regs.rat_type;
            } else if (reg_status.cs_regs_valid && reg_status.cs_regs.regs_state == NWY_NW_SERVICE_FULL) {
                *rat = reg_status.cs_regs.rat_type;
            } else {
                *rat = NWY_NW_RAT_NONE;
            }
        }
        return true;
    }
    return false;
}

bool nwy_hal_net_get_ip(int sim_id, char *ip_out, int max_len) {
    if (!ip_out || max_len <= 0) return false;
    nwy_data_callinfo_t call_info;
    memset(&call_info, 0, sizeof(call_info));
    if (nwy_data_call_info_get(1, &call_info) == 0) { // cid 1
        if (strlen(call_info.ipv4_str) > 0) {
            strncpy(ip_out, call_info.ipv4_str, max_len - 1);
            ip_out[max_len - 1] = '\0';
            return true;
        }
    }
    return false;
}
