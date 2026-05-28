#include <stdlib.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"
#include "nwy_gpio_api.h"

// Define the GPIO pin to use. 
// Change 9 to your specific LED/GPIO pin number.
#define BLINK_GPIO_ID 9

nwy_osi_thread_t blinky_thread = NULL;

static void blinky_func(void *param)
{
    nwy_gpio_direction_set(BLINK_GPIO_ID, PIN_DIRECTION_OUT);
    
    NWY_SDK_LOG_DEBUG("Blinky started on GPIO %d", BLINK_GPIO_ID);
    
    int state = 0;
    while(1) {
        state = !state;
        nwy_gpio_value_set(BLINK_GPIO_ID, state ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
        
        NWY_SDK_LOG_DEBUG("Blinky GPIO %d: %s", BLINK_GPIO_ID, state ? "HIGH" : "LOW");
        
        nwy_thread_sleep(1000);
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
    nwy_thread_sleep(5 * 1000);
    NWY_SDK_LOG_DEBUG("Blinky Application Entered...");
    
    nwy_thread_create(&blinky_thread, "blinky", NWY_OSI_PRIORITY_NORMAL, blinky_func, NULL, 0, 1024 * 4, NULL);
    
    return 0;
}

void appimg_exit(void)
{
    NWY_SDK_LOG_DEBUG("Blinky Application Exited");
}
