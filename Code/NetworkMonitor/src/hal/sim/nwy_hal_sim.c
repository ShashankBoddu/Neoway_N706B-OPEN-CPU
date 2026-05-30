#include "hal/sim/nwy_hal_sim.h"
#include "nwy_sim_api.h"
#include <string.h>

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
    if (!imsi_out) return false;
    return (nwy_sim_imsi_get(get_nwy_sim_id(sim_id), imsi_out, 32) == 0);
}

bool nwy_hal_sim_get_iccid(int sim_id, char *iccid_out) {
    if (!iccid_out) return false;
    return (nwy_sim_iccid_get(get_nwy_sim_id(sim_id), iccid_out, 32) == 0);
}
