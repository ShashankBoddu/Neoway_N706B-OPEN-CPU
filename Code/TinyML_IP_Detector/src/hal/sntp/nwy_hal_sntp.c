#include "hal/sntp/nwy_hal_sntp.h"
#include "nwy_sntp_api.h"
#include <string.h>

static nwy_hal_sntp_sync_cb_t g_user_sntp_cb = NULL;

static void internal_sntp_callback(nwy_sntp_result_type *result) {
    if (g_user_sntp_cb) {
        bool success = (result != NULL && result->event == NWY_SNTP_SUCCESS);
        g_user_sntp_cb(success);
    }
}

bool nwy_hal_sntp_sync_time(int cid, const char* ntp_server, const char* tz, nwy_hal_sntp_sync_cb_t cb) {
    if (!ntp_server || !tz) return false;

    g_user_sntp_cb = cb;

    nwy_sntp_param_t param;
    memset(&param, 0, sizeof(param));
    
    param.cid = cid; 
    strncpy(param.url, ntp_server, NWY_SNTP_URL_MAX_LEN - 1);
    param.timeout = 10;
    param.retry_times = 3;
    strncpy(param.tz, tz, NWY_SNTP_TZ_MAX_LEN - 1);
    param.dst = 0; // Standard is 0 (No DST)

    return (nwy_sntp_get_time(&param, internal_sntp_callback) == 0);
}
