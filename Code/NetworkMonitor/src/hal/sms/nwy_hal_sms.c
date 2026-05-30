#include "hal/sms/nwy_hal_sms.h"
#include "nwy_sms_api.h"
#include "nwy_sim_api.h"
#include <string.h>
#include <stdio.h>

static nwy_hal_sms_recv_callback_t g_user_sms_cb = NULL;

static nwy_sim_id_e get_nwy_sim_id(int sim_id) {
    if (sim_id == 2) {
        return NWY_SIM_ID_SLOT_2;
    }
    return NWY_SIM_ID_SLOT_1;
}

static void internal_sms_handler(nwy_sim_id_e sim_id, nwy_mt_sms_event_e urc_type, nwy_sms_info_ind_t *ind_struct) {
    if (!ind_struct) return;
    
    if (urc_type == NWY_SMS_PP_IND) {
        if (g_user_sms_cb) {
            char phone_num[32];
            char message[NWY_SMS_MAX_MT_MSG_LENGTH + 1];
            char timestamp[32];
            
            // Extract phone number
            strncpy(phone_num, ind_struct->sms_info.source_phone_num, sizeof(phone_num) - 1);
            phone_num[sizeof(phone_num) - 1] = '\0';
            
            // Extract text message
            const char *msg_ptr = "";
            if (strlen((char*)ind_struct->sms_info.msg_decoded_content) > 0) {
                msg_ptr = (char*)ind_struct->sms_info.msg_decoded_content;
            } else {
                msg_ptr = (char*)ind_struct->sms_info.msg_content;
            }
            strncpy(message, msg_ptr, sizeof(message) - 1);
            message[sizeof(message) - 1] = '\0';
            
            // Format timestamp (checking for valid timezone offset if needed, or simple string)
            snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                     ind_struct->sms_info.date.uYear,
                     ind_struct->sms_info.date.uMonth,
                     ind_struct->sms_info.date.uDay,
                     ind_struct->sms_info.date.uHour,
                     ind_struct->sms_info.date.uMinute,
                     ind_struct->sms_info.date.uSecond);
            
            g_user_sms_cb(phone_num, message, timestamp);
        }
    }
}

bool nwy_hal_sms_init(int sim_id) {
    nwy_sim_id_e internal_sim = get_nwy_sim_id(sim_id);
    
    // 1. Initialize SMS options
    if (nwy_sms_option_init(internal_sim) != 0) {
        return false;
    }
    
    // 2. Set SMS storage to UIM (SIM card) as standard
    if (nwy_sms_storage_set(internal_sim, NWY_SMS_STORAGE_TYPE_UIM) != 0) {
        return false;
    }
    
    // 3. Configure SMS reporting to store message and notify AP directly
    nwy_sms_report_para_t report_para;
    memset(&report_para, 0, sizeof(report_para));
    report_para.transfer_type = NWY_SMS_TRANSFER_AND_STORE;
    report_para.transfer_online_mode = NWY_SMS_TRANSFER_ONLINE_DIRECT;
    if (nwy_sms_report_set(internal_sim, report_para) != 0) {
        return false;
    }
    
    return true;
}

bool nwy_hal_sms_register_recv_cb(int sim_id, nwy_hal_sms_recv_callback_t cb) {
    g_user_sms_cb = cb;
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
    
    sms_info.msg_format = NWY_SMS_MSG_FORMAT_GSM7; // Default to GSM7 for general text
    
    return (nwy_sms_msg_send(get_nwy_sim_id(sim_id), &sms_info) == 0);
}

bool nwy_hal_sms_delete(int sim_id, int index) {
    return (nwy_sms_msg_del(get_nwy_sim_id(sim_id), (uint16_t)index) == 0);
}
