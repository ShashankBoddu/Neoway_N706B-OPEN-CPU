#ifndef __NWY_HAL_NET_H__
#define __NWY_HAL_NET_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback definition for data call status changes
 * @param connected true if connected, false if disconnected
 * @param ip_address String containing the IPv4 address if connected
 */
typedef void (*nwy_hal_net_callback_t)(bool connected, const char *ip_address);

/**
 * @brief Check if the module is registered to the Cellular Network (PS/CS)
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @return true if registered
 */
bool nwy_hal_net_is_registered(int sim_id);

/**
 * @brief Get current cellular signal strength
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param rssi Pointer to store RSSI value
 * @return true if successful
 */
bool nwy_hal_net_get_signal(int sim_id, int *rssi);

/**
 * @brief Get CSQ signal quality and bit error rate
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param csq Out: CSQ level (0-31, 99)
 * @param ber Out: Bit error rate (0-7, 99)
 * @return true if successful
 */
bool nwy_hal_net_get_csq(int sim_id, int *csq, int *ber);

/**
 * @brief Start a data call asynchronously. Registers the necessary modem callbacks safely.
 * @param sim_id 1 for SLOT_1
 * @param cb Callback to invoke when connection state changes
 * @return true if connection initiation was successful
 */
bool nwy_hal_net_start_data_call(int sim_id, nwy_hal_net_callback_t cb);

/**
 * @brief Disconnect an active data call
 * @param sim_id 1 for SLOT_1
 */
void nwy_hal_net_stop_data_call(int sim_id);

/**
 * @brief Get the IPv4 address of the active data call
 * @param sim_id 1 for SLOT_1
 * @param ip_out Buffer to store IP string (at least 16 bytes)
 * @param max_len Size of ip_out buffer
 * @return true if successful and IP is valid
 */
bool nwy_hal_net_get_ip(int sim_id, char *ip_out, int max_len);

/**
 * @brief Get registered operator name
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param name_out Buffer to store operator name
 * @param max_len Size of name_out buffer
 * @return true if successful
 */
bool nwy_hal_net_get_operator_name(int sim_id, char *name_out, int max_len);

/**
 * @brief Get detailed registration info
 * @param sim_id 1 for SLOT_1, 2 for SLOT_2
 * @param cs_state Out: CS registration state (nwy_nw_service_type_e)
 * @param ps_state Out: PS registration state (nwy_nw_service_type_e)
 * @param rat Out: Radio Access Technology type (nwy_nw_rat_type_e)
 * @return true if successful
 */
bool nwy_hal_net_get_registration_details(int sim_id, int *cs_state, int *ps_state, int *rat);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_NET_H__
