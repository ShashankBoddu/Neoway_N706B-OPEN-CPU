#ifndef __NWY_HAL_NET_H__
#define __NWY_HAL_NET_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_network_api.h"
#include "nwy_data_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback definition for data call status changes
 */
typedef void (*nwy_hal_net_callback_t)(int sim_id, int profile_idx, bool connected, const char *ip_address);

// SIM translation helper
nwy_sim_id_e nwy_hal_get_sim_id_enum(int sim_id);

// Basic Network Info & Registration
bool nwy_hal_net_is_registered(int sim_id);
bool nwy_hal_net_get_signal(int sim_id, int *rssi);
bool nwy_hal_net_get_csq(int sim_id, int *csq, int *ber);
bool nwy_hal_net_get_operator_name(int sim_id, char *name_out, int max_len);
bool nwy_hal_net_get_registration_details(int sim_id, int *cs_state, int *ps_state, int *rat);
bool nwy_hal_net_get_operator_details(int sim_id, nwy_nw_operator_t *oper_info);

// Network Mode Configuration
bool nwy_hal_net_get_mode(int sim_id, int *network_mode);
bool nwy_hal_net_set_mode(int sim_id, nwy_nw_rat_type_e mode);

// RF Band Configuration
bool nwy_hal_net_get_priband(int sim_id, nwy_nw_priband_t *priband);
bool nwy_hal_net_set_priband(int sim_id, nwy_nw_priband_t *priband);

// IMS / VoLTE Configuration
bool nwy_hal_net_get_ims_mode(int sim_id, uint8_t *ims_mode);
bool nwy_hal_net_set_ims_mode(int sim_id, uint8_t ims_mode);

// Radio State Control
bool nwy_hal_net_get_radio_mode(nwy_nw_radio_mode_e *radio_mode);
bool nwy_hal_net_set_radio_mode(nwy_nw_radio_mode_e radio_mode);

// PSM & eDRX Configurations
bool nwy_hal_net_get_psm_info(int sim_id, nwy_nw_psm_info_t *psm_info);
bool nwy_hal_net_set_psm_info(int sim_id, nwy_nw_psm_info_t *psm_info);

// Custom configurations
bool nwy_hal_net_get_custom_cfg(int sim_id, nwy_nw_config_type_e cfg_option, nwy_nw_config_info_u *cfg_info);
bool nwy_hal_net_set_custom_cfg(int sim_id, nwy_nw_config_type_e cfg_option, nwy_nw_config_info_u *cfg_info);

// Diagnostics & Cell Info
bool nwy_hal_net_get_signal_info(int sim_id, nwy_nw_signal_info_t *info);
bool nwy_hal_net_get_cellinfo(int sim_id, nwy_nw_cellinfo_mode_e scan_mode, nwy_nw_cellinfo_t *info);

// Data connection APIs
bool nwy_hal_net_start_data_call(int sim_id, int profile_idx, nwy_hal_net_callback_t cb);
void nwy_hal_net_stop_data_call(int sim_id, int profile_idx);
bool nwy_hal_net_get_ip(int sim_id, int profile_idx, char *ip_out, int max_len);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_NET_H__
