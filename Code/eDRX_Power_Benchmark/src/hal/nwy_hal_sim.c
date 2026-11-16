#include "hal/nwy_hal_sim.h"
#include <string.h>

static nwy_hal_sim_urc_callback_t g_user_sim_cb[2] = {NULL};

static nwy_sim_id_e get_nwy_sim_id(int sim_id) {
  if (sim_id == 2) {
    return NWY_SIM_ID_SLOT_2;
  }
  return NWY_SIM_ID_SLOT_1;
}

bool nwy_hal_sim_is_ready(int sim_id) {
  nwy_sim_status_e status;
  if (nwy_sim_status_get(get_nwy_sim_id(sim_id), &status) == 0) {
    return (status == NWY_SIM_READY);
  }
  return false;
}

bool nwy_hal_sim_get_imsi(int sim_id, char *imsi_out) {
  if (!imsi_out)
    return false;
  return (nwy_sim_imsi_get(get_nwy_sim_id(sim_id), imsi_out, 32) == 0);
}

bool nwy_hal_sim_get_iccid(int sim_id, char *iccid_out) {
  if (!iccid_out)
    return false;
  return (nwy_sim_iccid_get(get_nwy_sim_id(sim_id), iccid_out, 32) == 0);
}

bool nwy_hal_sim_get_msisdn(int sim_id, char *msisdn_out, int max_len) {
  if (!msisdn_out || max_len <= 0)
    return false;
  return (nwy_sim_msisdn_get(get_nwy_sim_id(sim_id), msisdn_out, max_len) == 0);
}

bool nwy_hal_sim_verify_pin(int sim_id, const char *pin) {
  if (!pin)
    return false;
  return (nwy_sim_pin_verify(get_nwy_sim_id(sim_id), (char *)pin) == 0);
}

bool nwy_hal_sim_get_pin_mode(int sim_id, nwy_sim_pin_mode_e *pin_mode) {
  if (!pin_mode)
    return false;
  return (nwy_sim_pin_mode_get(get_nwy_sim_id(sim_id), pin_mode) == 0);
}

bool nwy_hal_sim_enable_pin(int sim_id, const char *pin) {
  if (!pin)
    return false;
  return (nwy_sim_pin_enable(get_nwy_sim_id(sim_id), (char *)pin) == 0);
}

bool nwy_hal_sim_disable_pin(int sim_id, const char *pin) {
  if (!pin)
    return false;
  return (nwy_sim_pin_disable(get_nwy_sim_id(sim_id), (char *)pin) == 0);
}

bool nwy_hal_sim_change_pin(int sim_id, const char *old_pin,
                            const char *new_pin) {
  if (!old_pin || !new_pin)
    return false;
  return (nwy_sim_pin_change(get_nwy_sim_id(sim_id), (char *)old_pin,
                             (char *)new_pin) == 0);
}

bool nwy_hal_sim_verify_puk(int sim_id, const char *puk, const char *new_pin) {
  if (!puk || !new_pin)
    return false;
  return (nwy_sim_pin_unlock(get_nwy_sim_id(sim_id), (char *)puk,
                             (char *)new_pin) == 0);
}

bool nwy_hal_sim_get_retry_times(int sim_id, uint8_t *pin_attempts,
                                 uint8_t *puk_attempts) {
  if (!pin_attempts || !puk_attempts)
    return false;
  return (nwy_sim_pin_times_get(get_nwy_sim_id(sim_id), pin_attempts,
                                puk_attempts) == 0);
}

bool nwy_hal_sim_get_active_slot(uint8_t *slot_id) {
  if (!slot_id)
    return false;
  return (nwy_sim_slot_get(slot_id) == 0);
}

bool nwy_hal_sim_set_active_slot(uint8_t slot_id, uint8_t save_to_nv) {
  return (nwy_sim_slot_set(slot_id, save_to_nv) == 0);
}

bool nwy_hal_sim_set_detect(int sim_id, uint8_t detect_mode,
                            uint8_t trigger_mode) {
  return (nwy_sim_detect_set(sim_id - 1, detect_mode, trigger_mode) == 0);
}

bool nwy_hal_sim_power_set(int sim_id, int power_mode) {
  return (nwy_sim_power_set(get_nwy_sim_id(sim_id), power_mode) == 0);
}

bool nwy_hal_sim_reset(int sim_id) {
  return (nwy_sim_reset(get_nwy_sim_id(sim_id)) == 0);
}

bool nwy_hal_sim_csim_command(int sim_id, const char *command, uint8_t length,
                              char *response_out, int response_max_len) {
  if (!command || !response_out || response_max_len <= 0)
    return false;
  return (nwy_sim_csim(get_nwy_sim_id(sim_id), (char *)command, length,
                       response_out, response_max_len) == 0);
}

static void internal_sim_cb(nwy_sim_id_e sim_id, nwy_sim_urc_type_e urc_type,
                            nwy_sim_info_ind_t *ind_struct) {
  int user_sim_id = (sim_id == NWY_SIM_ID_SLOT_2) ? 2 : 1;
  int sim_idx = user_sim_id - 1;

  if (sim_idx < 0 || sim_idx >= 2)
    return;

  if (g_user_sim_cb[sim_idx] && urc_type == NWY_SIM_URC_TYPE_STATUS) {
    bool card_present = (ind_struct->sim_detect_status == 1);
    g_user_sim_cb[sim_idx](user_sim_id, card_present, ind_struct->sim_status);
  }
}

bool nwy_hal_sim_register_urc_cb(int sim_id, nwy_hal_sim_urc_callback_t cb) {
  int sim_idx = sim_id - 1;
  if (sim_idx < 0 || sim_idx >= 2)
    return false;

  g_user_sim_cb[sim_idx] = cb;
  return (nwy_sim_urc_reg(get_nwy_sim_id(sim_id), internal_sim_cb) == 0);
}

bool nwy_hal_sim_unregister_urc_cb(int sim_id) {
  int sim_idx = sim_id - 1;
  if (sim_idx < 0 || sim_idx >= 2)
    return false;

  g_user_sim_cb[sim_idx] = NULL;
  return (nwy_sim_urc_unreg(get_nwy_sim_id(sim_id), internal_sim_cb) == 0);
}
