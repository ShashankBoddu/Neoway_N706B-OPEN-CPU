#ifndef __NWY_HAL_GPIO_H__
#define __NWY_HAL_GPIO_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Common LEDs for Neoway N706B OpenCPU EVB/Custom Board
#define HAL_GPIO_NET_STATUS 69 // NET LED
#define HAL_GPIO_STATUS     70 // STATUS LED

/**
 * @brief Initialize a GPIO as output with an initial state
 * @param gpio_id The GPIO ID
 * @param initial_high If true, starts HIGH, otherwise LOW
 * @return true if successful
 */
bool nwy_hal_gpio_init_out(uint32_t gpio_id, bool initial_high);

/**
 * @brief Set the state of an output GPIO
 * @param gpio_id The GPIO ID
 * @param high true for HIGH, false for LOW
 * @return true if successful
 */
bool nwy_hal_gpio_set_value(uint32_t gpio_id, bool high);

/**
 * @brief Get the state of a GPIO
 * @param gpio_id The GPIO ID
 * @return true if HIGH, false if LOW
 */
bool nwy_hal_gpio_get_value(uint32_t gpio_id);

/**
 * @brief Toggle the state of an output GPIO
 * @param gpio_id The GPIO ID
 * @return true if successful
 */
bool nwy_hal_gpio_toggle(uint32_t gpio_id);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_GPIO_H__
