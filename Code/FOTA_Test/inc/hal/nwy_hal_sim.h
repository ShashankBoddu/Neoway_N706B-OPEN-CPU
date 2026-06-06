#ifndef __NWY_HAL_SIM_H__
#define __NWY_HAL_SIM_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_sim_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// SIM Card insertion events callback
typedef void (*nwy_hal_sim_urc_callback_t)(int sim_id, bool card_present, nwy_sim_status_e sim_status);

// Readiness & basic identification
bool nwy_hal_sim_is_ready(int sim_id);
bool nwy_hal_sim_get_imsi(int sim_id, char *imsi_out);
bool nwy_hal_sim_get_iccid(int sim_id, char *iccid_out);
bool nwy_hal_sim_get_msisdn(int sim_id, char *msisdn_out, int max_len);

// PIN / PUK Lock Control
bool nwy_hal_sim_verify_pin(int sim_id, const char *pin);
bool nwy_hal_sim_get_pin_mode(int sim_id, nwy_sim_pin_mode_e *pin_mode);
bool nwy_hal_sim_enable_pin(int sim_id, const char *pin);
bool nwy_hal_sim_disable_pin(int sim_id, const char *pin);
bool nwy_hal_sim_change_pin(int sim_id, const char *old_pin, const char *new_pin);
bool nwy_hal_sim_verify_puk(int sim_id, const char *puk, const char *new_pin);
bool nwy_hal_sim_get_retry_times(int sim_id, uint8_t *pin_attempts, uint8_t *puk_attempts);

// Dual-SIM & slot control
bool nwy_hal_sim_get_active_slot(uint8_t *slot_id);
bool nwy_hal_sim_set_active_slot(uint8_t slot_id, uint8_t save_to_nv);

// SIM Hardware pins & power control
bool nwy_hal_sim_set_detect(int sim_id, uint8_t detect_mode, uint8_t trigger_mode);
bool nwy_hal_sim_power_set(int sim_id, int power_mode); // 0: power down, 1: power up
bool nwy_hal_sim_reset(int sim_id);

// Smart Card raw APDU channel
bool nwy_hal_sim_csim_command(int sim_id, const char *command, uint8_t length, char *response_out, int response_max_len);

// URC callback registration
bool nwy_hal_sim_register_urc_cb(int sim_id, nwy_hal_sim_urc_callback_t cb);
bool nwy_hal_sim_unregister_urc_cb(int sim_id);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SIM_H__
