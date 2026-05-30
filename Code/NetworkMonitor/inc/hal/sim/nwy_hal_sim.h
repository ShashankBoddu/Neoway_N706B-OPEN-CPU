#ifndef __NWY_HAL_SIM_H__
#define __NWY_HAL_SIM_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if the SIM card is inserted and ready
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @return true if SIM is ready
 */
bool nwy_hal_sim_is_ready(int sim_id);

/**
 * @brief Get the IMSI of the SIM card
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param imsi_out Buffer to store IMSI (at least 16 bytes)
 * @return true if successful
 */
bool nwy_hal_sim_get_imsi(int sim_id, char *imsi_out);

/**
 * @brief Get the ICCID of the SIM card
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param iccid_out Buffer to store ICCID (at least 21 bytes)
 * @return true if successful
 */
bool nwy_hal_sim_get_iccid(int sim_id, char *iccid_out);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SIM_H__
