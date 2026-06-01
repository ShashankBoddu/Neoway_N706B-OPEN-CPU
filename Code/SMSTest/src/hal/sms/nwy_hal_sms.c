#include "hal/sms/nwy_hal_sms.h"
#include "nwy_sms_api.h"
#include "nwy_sim_api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static nwy_hal_sms_recv_callback_t g_user_sms_cb[2] = { NULL }; 

static nwy_sim_id_e get_nwy_sim_id(int sim_id) {
    if (sim_id == 2) {
        return NWY_SIM_ID_SLOT_2;
    }
    return NWY_SIM_ID_SLOT_1;
}

static void internal_sms_handler(nwy_sim_id_e sim_id, nwy_mt_sms_event_e urc_type, nwy_sms_info_ind_t *ind_struct) {
    if (!ind_struct) return;
    
    int user_sim_id = (sim_id == NWY_SIM_ID_SLOT_2) ? 2 : 1;
    int sim_idx = user_sim_id - 1;
    
    if (sim_idx < 0 || sim_idx >= 2) {
        return;
    }
    
    // Only handle point-to-point incoming messages that were stored
    if (urc_type == NWY_SMS_PP_IND) {
        if (g_user_sms_cb[sim_idx]) {
            // DO NOT read or send SMS here! URC contexts must not block IPC.
            // Just pass the index to the application thread.
            g_user_sms_cb[sim_idx](user_sim_id, ind_struct->sms_info.nIndex);
        }
    }
}

bool nwy_hal_sms_init(int sim_id) {
    nwy_sim_id_e internal_sim = get_nwy_sim_id(sim_id);
    
    if (nwy_sms_option_init(internal_sim) != 0) {
        return false;
    }
    
    if (nwy_sms_storage_set(internal_sim, NWY_SMS_STORAGE_TYPE_UIM) != 0) {
        return false;
    }
    
    nwy_sms_report_para_t report_para;
    memset(&report_para, 0, sizeof(report_para));
    report_para.transfer_type = NWY_SMS_TRANSFER_AND_STORE;
    report_para.transfer_online_mode = NWY_SMS_TRANSFER_ONLINE_DIRECT;
    if (nwy_sms_report_set(internal_sim, report_para) != 0) {
        return false;
    }
    
    nwy_sms_msg_del_ext(internal_sim, NWY_SMS_MSG_DFLAG_ALL);
    
    return true;
}

bool nwy_hal_sms_register_recv_cb(int sim_id, nwy_hal_sms_recv_callback_t cb) {
    int sim_idx = sim_id - 1;
    if (sim_idx < 0 || sim_idx >= 2) {
        return false;
    }
    g_user_sms_cb[sim_idx] = cb;
    return (nwy_sms_recv_cb_reg(get_nwy_sim_id(sim_id), internal_sms_handler) == 0);
}

bool nwy_hal_sms_send(int sim_id, const char *phone_num, const char *message) {
    if (!phone_num || !message) return false;
    
    nwy_sms_info_type_t sms_info;
    memset(&sms_info, 0, sizeof(sms_info));
    
    strncpy(sms_info.phone_num, phone_num, sizeof(sms_info.phone_num) - 1);
    sms_info.phone_num[sizeof(sms_info.phone_num) - 1] = '\0';
    
    strncpy(sms_info.msg_context, message, sizeof(sms_info.msg_context) - 1);
    sms_info.msg_context[sizeof(sms_info.msg_context) - 1] = '\0';
    sms_info.msg_context_len = strlen(sms_info.msg_context);
    
    sms_info.msg_format = NWY_SMS_MSG_FORMAT_GSM7; 
    
    return (nwy_sms_msg_send(get_nwy_sim_id(sim_id), &sms_info) == 0);
}

bool nwy_hal_sms_read(int sim_id, int index, nwy_sms_recv_info_type_t *sms_data_out) {
    if (!sms_data_out) return false;
    return (nwy_sms_msg_read(get_nwy_sim_id(sim_id), (uint16_t)index, sms_data_out) == 0);
}

bool nwy_hal_sms_delete(int sim_id, int index) {
    return (nwy_sms_msg_del(get_nwy_sim_id(sim_id), (uint16_t)index) == 0);
}

bool nwy_hal_sms_delete_by_type(int sim_id, nwy_sms_msg_dflag_e delflag) {
    return (nwy_sms_msg_del_ext(get_nwy_sim_id(sim_id), delflag) == 0);
}

bool nwy_hal_sms_get_sca(int sim_id, nwy_sms_sca_t *sca) {
    if (!sca) return false;
    return (nwy_sms_sca_get(get_nwy_sim_id(sim_id), sca) == 0);
}

bool nwy_hal_sms_set_sca(int sim_id, nwy_sms_sca_t sca) {
    return (nwy_sms_sca_set(get_nwy_sim_id(sim_id), sca) == 0);
}

bool nwy_hal_sms_get_storage(int sim_id, nwy_sms_storage_type_e *storage) {
    if (!storage) return false;
    return (nwy_sms_storage_get(get_nwy_sim_id(sim_id), storage) == 0);
}

bool nwy_hal_sms_set_storage(int sim_id, nwy_sms_storage_type_e storage) {
    return (nwy_sms_storage_set(get_nwy_sim_id(sim_id), storage) == 0);
}

bool nwy_hal_sms_set_report_mode(int sim_id, nwy_sms_report_para_t report_para) {
    return (nwy_sms_report_set(get_nwy_sim_id(sim_id), report_para) == 0);
}

bool nwy_hal_sms_list_indices(int sim_id, nwy_sms_msg_list_t *sms_list_out) {
    if (!sms_list_out) return false;
    return (nwy_sms_msg_list_read(get_nwy_sim_id(sim_id), sms_list_out) == 0);
}
