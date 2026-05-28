#include <stdlib.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"

nwy_osi_thread_t hello_world_thread = NULL;

static void hello_world_func(void *param)
{
    while(1) {
        NWY_SDK_LOG_DEBUG("Hello World from Neoway N706B OpenCPU!");
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
    NWY_SDK_LOG_DEBUG("Application Entered...");
    
    nwy_thread_create(&hello_world_thread, "hello_world", NWY_OSI_PRIORITY_NORMAL, hello_world_func, NULL, 0, 1024 * 4, NULL);
    
    return 0;
}

void appimg_exit(void)
{
    NWY_SDK_LOG_DEBUG("Application Exited", 0, 0, 0);
}
