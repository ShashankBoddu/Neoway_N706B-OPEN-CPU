#ifndef __NWY_HAL_SMS_H__
#define __NWY_HAL_SMS_H__

#include "nwy_sms_api.h"
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nwy_hal_sms_recv_callback_t)(int sim_id, int sms_index);

// Setup & Callbacks
bool nwy_hal_sms_init(int sim_id, nwy_sms_storage_type_e sms_storage);
bool nwy_hal_sms_register_recv_cb(int sim_id, nwy_hal_sms_recv_callback_t cb);

// Message Transmission & Deletion
bool nwy_hal_sms_send(int sim_id, const char *phone_num, const char *message);
bool nwy_hal_sms_read(int sim_id, int index,
                      nwy_sms_recv_info_type_t *sms_data_out);
bool nwy_hal_sms_delete(int sim_id, int index);
bool nwy_hal_sms_delete_by_type(int sim_id, nwy_sms_msg_dflag_e delflag);

// SMS Configurations
bool nwy_hal_sms_get_sca(int sim_id, nwy_sms_sca_t *sca);
bool nwy_hal_sms_set_sca(int sim_id, nwy_sms_sca_t sca);
bool nwy_hal_sms_get_storage(int sim_id, nwy_sms_storage_type_e *storage);
bool nwy_hal_sms_set_storage(int sim_id, nwy_sms_storage_type_e storage);
bool nwy_hal_sms_set_report_mode(int sim_id, nwy_sms_report_para_t report_para);

// Scan / Query Lists
bool nwy_hal_sms_list_indices(int sim_id, nwy_sms_msg_list_t *sms_list_out);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SMS_H__
