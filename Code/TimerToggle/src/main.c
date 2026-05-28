#include <stdlib.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"
#include "nwy_gpio_api.h"

#define TOGGLE_GPIO_ID 9
#define TIMER_INTERVAL_MS 500

static nwy_osi_timer_t toggle_timer = NULL;
static int led_state = 0;

static void timer_callback(void *ctx)
{
    led_state = !led_state;
    nwy_gpio_value_set(TOGGLE_GPIO_ID, led_state ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
    
    NWY_SDK_LOG_DEBUG("Timer Toggle: GPIO %d set to %s", TOGGLE_GPIO_ID, led_state ? "HIGH" : "LOW");
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
    nwy_thread_sleep(5000); // Wait for system stability
    NWY_SDK_LOG_DEBUG("Timer Toggle Application Started...");
    
    // Initialize GPIO
    nwy_gpio_direction_set(TOGGLE_GPIO_ID, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(TOGGLE_GPIO_ID, PIN_LEVEL_LOW);
    
    // Setup and start periodic timer
    nwy_timer_para_t timer_para;
    timer_para.expired_time = TIMER_INTERVAL_MS;
    timer_para.type = NWY_TIMER_PERIODIC;
    timer_para.thread_hdl = NWY_TIMER_IN_SERVICE; // Run in system timer thread
    timer_para.cb = timer_callback;
    timer_para.cb_para = NULL;
    
    nwy_error_e ret = nwy_sdk_timer_create(&toggle_timer, &timer_para);
    if (ret == NWY_SUCCESS && toggle_timer != NULL) {
        nwy_sdk_timer_start(toggle_timer, &timer_para);
        NWY_SDK_LOG_DEBUG("Timer started with %dms interval", TIMER_INTERVAL_MS);
    } else {
        NWY_SDK_LOG_DEBUG("Failed to create timer, error: %d", ret);
    }
    
    return 0;
}

void appimg_exit(void)
{
    NWY_SDK_LOG_DEBUG("Timer Toggle Application Exited");
    if (toggle_timer != NULL) {
        nwy_sdk_timer_stop(toggle_timer);
        nwy_sdk_timer_destory(toggle_timer);
    }
}
