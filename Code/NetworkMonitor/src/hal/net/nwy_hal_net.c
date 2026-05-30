#include "hal/net/nwy_hal_net.h"
#include <string.h>
#include <stdio.h>

static nwy_hal_net_callback_t g_user_net_cb[2][7] = { { NULL } }; 

nwy_sim_id_e nwy_hal_get_sim_id_enum(int sim_id) {
    if (sim_id == 2) {
        return NWY_SIM_ID_SLOT_2;
    }
    return NWY_SIM_ID_SLOT_1;
}

static void handle_internal_data_cb(int sim_id, int profile_idx, nwy_data_call_state_e ind_state) {
    int sim_idx = sim_id - 1; 
    if (sim_idx < 0 || sim_idx >= 2 || profile_idx < 1 || profile_idx > 6) {
        return;
    }
    
    nwy_hal_net_callback_t cb = g_user_net_cb[sim_idx][profile_idx];
    if (cb) {
        if (ind_state == NWY_DATA_CALL_CONNECTED_STATE) {
            nwy_data_callinfo_t call_info;
            memset(&call_info, 0, sizeof(call_info));
            nwy_data_call_info_get(profile_idx, &call_info);
            cb(sim_id, profile_idx, true, call_info.ipv4_str);
        } else {
            cb(sim_id, profile_idx, false, NULL);
        }
    }
}

static void internal_data_cb_sim1(int profile_idx, nwy_data_call_state_e ind_state) {
    handle_internal_data_cb(1, profile_idx, ind_state);
}

static void internal_data_cb_sim2(int profile_idx, nwy_data_call_state_e ind_state) {
    handle_internal_data_cb(2, profile_idx, ind_state);
}

bool nwy_hal_net_is_registered(int sim_id) {
    nwy_nw_regstatus_t reg_status;
    memset(&reg_status, 0, sizeof(reg_status));
    
    if (nwy_nw_regstatus_get(nwy_hal_get_sim_id_enum(sim_id), &reg_status) == 0) {
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
    if (nwy_nw_csq_get(nwy_hal_get_sim_id_enum(sim_id), &sig) == 0) {
        *rssi = sig.csq_rssi_level;
        return true;
    }
    return false;
}

bool nwy_hal_net_get_csq(int sim_id, int *csq, int *ber) {
    if (!csq || !ber) return false;
    nwy_nw_get_csq_info_t sig;
    if (nwy_nw_csq_get(nwy_hal_get_sim_id_enum(sim_id), &sig) == 0) {
        *csq = sig.csq_rssi_level;
        *ber = sig.ber;
        return true;
    }
    return false;
}

bool nwy_hal_net_get_operator_name(int sim_id, char *name_out, int max_len) {
    if (!name_out || max_len <= 0) return false;
    nwy_nw_operator_t opt;
    memset(&opt, 0, sizeof(opt));
    if (nwy_nw_operator_get(nwy_hal_get_sim_id_enum(sim_id), &opt) == 0) {
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
    
    if (nwy_nw_regstatus_get(nwy_hal_get_sim_id_enum(sim_id), &reg_status) == 0) {
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

bool nwy_hal_net_get_operator_details(int sim_id, nwy_nw_operator_t *oper_info) {
    if (!oper_info) return false;
    return (nwy_nw_operator_get(nwy_hal_get_sim_id_enum(sim_id), oper_info) == 0);
}

bool nwy_hal_net_get_mode(int sim_id, int *network_mode) {
    if (!network_mode) return false;
    return (nwy_nw_mode_get(nwy_hal_get_sim_id_enum(sim_id), network_mode) == 0);
}

bool nwy_hal_net_set_mode(int sim_id, nwy_nw_rat_type_e mode) {
    return (nwy_nw_mode_set(nwy_hal_get_sim_id_enum(sim_id), mode) == 0);
}

bool nwy_hal_net_get_priband(int sim_id, nwy_nw_priband_t *priband) {
    if (!priband) return false;
    return (nwy_nw_priband_get(nwy_hal_get_sim_id_enum(sim_id), priband) == 0);
}

bool nwy_hal_net_set_priband(int sim_id, nwy_nw_priband_t *priband) {
    if (!priband) return false;
    return (nwy_nw_priband_set(nwy_hal_get_sim_id_enum(sim_id), priband) == 0);
}

bool nwy_hal_net_get_ims_mode(int sim_id, uint8_t *ims_mode) {
    if (!ims_mode) return false;
    return (nwy_nw_ims_get(nwy_hal_get_sim_id_enum(sim_id), ims_mode) == 0);
}

bool nwy_hal_net_set_ims_mode(int sim_id, uint8_t ims_mode) {
    return (nwy_nw_ims_set(nwy_hal_get_sim_id_enum(sim_id), ims_mode) == 0);
}

bool nwy_hal_net_get_radio_mode(nwy_nw_radio_mode_e *radio_mode) {
    if (!radio_mode) return false;
    return (nwy_nw_radio_get(radio_mode) == 0);
}

bool nwy_hal_net_set_radio_mode(nwy_nw_radio_mode_e radio_mode) {
    return (nwy_nw_radio_set(radio_mode) == 0);
}

bool nwy_hal_net_get_psm_info(int sim_id, nwy_nw_psm_info_t *psm_info) {
    if (!psm_info) return false;
    return (nwy_nw_psm_get(nwy_hal_get_sim_id_enum(sim_id), psm_info) == 0);
}

bool nwy_hal_net_set_psm_info(int sim_id, nwy_nw_psm_info_t *psm_info) {
    if (!psm_info) return false;
    return (nwy_nw_psm_set(nwy_hal_get_sim_id_enum(sim_id), psm_info) == 0);
}

bool nwy_hal_net_get_custom_cfg(int sim_id, nwy_nw_config_type_e cfg_option, nwy_nw_config_info_u *cfg_info) {
    if (!cfg_info) return false;
    return (nwy_nw_config_get(nwy_hal_get_sim_id_enum(sim_id), cfg_option, cfg_info) == 0);
}

bool nwy_hal_net_set_custom_cfg(int sim_id, nwy_nw_config_type_e cfg_option, nwy_nw_config_info_u *cfg_info) {
    if (!cfg_info) return false;
    return (nwy_nw_config_set(nwy_hal_get_sim_id_enum(sim_id), cfg_option, cfg_info) == 0);
}

bool nwy_hal_net_get_signal_info(int sim_id, nwy_nw_signal_info_t *info) {
    if (!info) return false;
    return (nwy_nw_signal_get(nwy_hal_get_sim_id_enum(sim_id), info) == 0);
}

bool nwy_hal_net_get_cellinfo(int sim_id, nwy_nw_cellinfo_mode_e scan_mode, nwy_nw_cellinfo_t *info) {
    if (!info) return false;
    return (nwy_nw_cellinfo_get(nwy_hal_get_sim_id_enum(sim_id), scan_mode, info) == 0);
}

bool nwy_hal_net_start_data_call(int sim_id, int profile_idx, nwy_hal_net_callback_t cb) {
    int sim_idx = sim_id - 1;
    if (sim_idx < 0 || sim_idx >= 2 || profile_idx < 1 || profile_idx > 6) {
        return false;
    }
    
    g_user_net_cb[sim_idx][profile_idx] = cb;
    nwy_data_reg_cb(nwy_hal_get_sim_id_enum(sim_id), profile_idx, (sim_id == 2) ? internal_data_cb_sim2 : internal_data_cb_sim1);
    
    nwy_data_start_call_t call_param;
    memset(&call_param, 0, sizeof(call_param));
    call_param.cid = profile_idx;
    call_param.action = NWY_DATA_CALL_ACT; 
    call_param.trigger_type = NWY_DATA_TRIGGER_OPEN;
    call_param.call_auto_type = NWY_DATA_CALL_AUTO_TYPE_ENABLE;
    call_param.set_profile.pdp_type = -1; 
    call_param.set_profile.auth_proto = -1; 
    call_param.if_internal_call = 1;
    
    return (nwy_data_call_start(nwy_hal_get_sim_id_enum(sim_id), &call_param) == 0);
}

void nwy_hal_net_stop_data_call(int sim_id, int profile_idx) {
    if (profile_idx < 1 || profile_idx > 6) {
        return;
    }
    nwy_data_start_call_t call_param;
    memset(&call_param, 0, sizeof(call_param));
    call_param.cid = profile_idx;
    call_param.action = NWY_DATA_CALL_DEACT; 
    call_param.trigger_type = NWY_DATA_TRIGGER_OPEN;
    call_param.if_internal_call = 1;
    
    nwy_data_call_start(nwy_hal_get_sim_id_enum(sim_id), &call_param);
}

bool nwy_hal_net_get_ip(int sim_id, int profile_idx, char *ip_out, int max_len) {
    if (!ip_out || max_len <= 0 || profile_idx < 1 || profile_idx > 6) return false;
    nwy_data_callinfo_t call_info;
    memset(&call_info, 0, sizeof(call_info));
    if (nwy_data_call_info_get(profile_idx, &call_info) == 0) {
        if (strlen(call_info.ipv4_str) > 0 && strcmp(call_info.ipv4_str, "0.0.0.0") != 0) {
            strncpy(ip_out, call_info.ipv4_str, max_len - 1);
            ip_out[max_len - 1] = '\0';
            return true;
        }
    }
    return false;
}
