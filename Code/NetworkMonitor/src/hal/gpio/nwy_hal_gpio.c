#include "hal/gpio/nwy_hal_gpio.h"
#include "nwy_gpio_api.h"

bool nwy_hal_gpio_init_out(uint32_t gpio_id, bool initial_high) {
    if (nwy_gpio_direction_set(gpio_id, PIN_DIRECTION_OUT) < 0) {
        return false;
    }
    return nwy_hal_gpio_set_value(gpio_id, initial_high);
}

bool nwy_hal_gpio_set_value(uint32_t gpio_id, bool high) {
    return (nwy_gpio_value_set(gpio_id, high ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW) >= 0);
}

bool nwy_hal_gpio_get_value(uint32_t gpio_id) {
    return (nwy_gpio_value_get(gpio_id) == 1);
}

bool nwy_hal_gpio_toggle(uint32_t gpio_id) {
    bool current = nwy_hal_gpio_get_value(gpio_id);
    return nwy_hal_gpio_set_value(gpio_id, !current);
}
