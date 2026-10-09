#include "hal/nwy_hal_sntp.h"
#include "hal/nwy_hal_os.h"
#include "hal/nwy_hal_uart.h"
#include "nwy_sntp_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SNTP_HAL"
#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) nwy_hal_uart_log_w(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

// Enterprise-grade fallback matrix to absorb operator drops or pooling
// resolution failures
static const char *g_sntp_fallback_servers[] = {
    "time.google.com", "time.cloudflare.com", "time.windows.com"};
#define SNTP_FALLBACK_COUNT                                                    \
  (sizeof(g_sntp_fallback_servers) / sizeof(g_sntp_fallback_servers[0]))

// Context state variable layout block to track state safely across async URC
// boundaries
typedef struct {
  int cid;
  char target_tz[10];
  int current_server_index; // -1 means primary string, >=0 means fallback array
                            // index
  char primary_server_url[128];
  nwy_hal_sntp_sync_cb_t user_cb;
  nwy_sntp_param_t inner_param;
} nwy_hal_sntp_context_t;

static nwy_hal_sntp_context_t g_sntp_ctx;
static void nwy_hal_sntp_execute_step(void);

/**
 * @brief Internal callback required by the core Neoway OpenCPU base firmware
 * engine. Handles incoming packets or timeout events dispatched by the cellular
 * stack.
 */
static void internal_sntp_callback(nwy_sntp_result_type *event) { //
  // Guard tracking pointer validation safely
  if (event != NULL && event->event == NWY_SNTP_SUCCESS) { //
    LOGI("System clock synchronized successfully.");
    if (g_sntp_ctx.user_cb) {
      g_sntp_ctx.user_cb(true);
    }
    return;
  }

  // Capture error state indicator if present
  int error_type = (event != NULL) ? (int)event->event : -1;

  // Fallback logic path entry: if primary failed, route requests to backup
  // clusters sequentially
  g_sntp_ctx.current_server_index++;
  if (g_sntp_ctx.current_server_index < (int)SNTP_FALLBACK_COUNT) {

    LOGW("Server attempt failed (Code:%d). Trying cascading backup index: %s",
         error_type, g_sntp_fallback_servers[g_sntp_ctx.current_server_index]);

    // Minor pacing delay to allow cellular LwIP socket handles to clear cleanly
    nwy_hal_os_thread_sleep(200);
    nwy_hal_sntp_execute_step();
  } else {
    // Exhausted fallback matrix options; notify the pipeline of completion
    // failure
    LOGE("All available NTP target clusters exhausted. Sync pipeline aborted.");
    if (g_sntp_ctx.user_cb) {
      g_sntp_ctx.user_cb(false);
    }
  }
}

/**
 * @brief Populates memory configurations and calls the core processing engine.
 */
static void nwy_hal_sntp_execute_step(void) {
  // Clear inner parameter structures to wipe away garbage stack markers
  // completely
  memset(&g_sntp_ctx.inner_param, 0, sizeof(nwy_sntp_param_t)); //

  g_sntp_ctx.inner_param.cid = g_sntp_ctx.cid; //
  g_sntp_ctx.inner_param.dst =
      0; // Explicitly disable daylight savings tracking features

  // Industrial safety thresholds: set structural limitations explicitly to
  // prevent hang blocks
  g_sntp_ctx.inner_param.timeout =
      20; // 20-second threshold gives sufficient cellular allowance
  g_sntp_ctx.inner_param.retry_times =
      4; // Request internal retransmissions before failing context

  // Safely copy bounded string arrays ensuring full string null termination
  // bounds
  strncpy(g_sntp_ctx.inner_param.tz, g_sntp_ctx.target_tz,
          sizeof(g_sntp_ctx.inner_param.tz) - 1);                          //
  g_sntp_ctx.inner_param.tz[sizeof(g_sntp_ctx.inner_param.tz) - 1] = '\0'; //

  const char *selected_url = NULL;
  if (g_sntp_ctx.current_server_index == -1) {
    selected_url = g_sntp_ctx.primary_server_url;
  } else {
    selected_url = g_sntp_fallback_servers[g_sntp_ctx.current_server_index];
  }

  strncpy(g_sntp_ctx.inner_param.url, selected_url,
          sizeof(g_sntp_ctx.inner_param.url) - 1);                           //
  g_sntp_ctx.inner_param.url[sizeof(g_sntp_ctx.inner_param.url) - 1] = '\0'; //

  // Route request parameters down to the base firmware engine
  nwy_error_e action_res =
      nwy_sntp_get_time(&g_sntp_ctx.inner_param, internal_sntp_callback); //
  if (action_res != NWY_SUCCESS) {
    // Handle instant pipeline rejection (e.g. invalid profile CID state) by
    // falling back immediately
    internal_sntp_callback(NULL);
  }
}

bool nwy_hal_sntp_sync_time(int cid, const char *ntp_server, const char *tz,
                            nwy_hal_sntp_sync_cb_t cb) {
  if (!ntp_server || !tz || cid < 1 || cid > 6) { //
    return false;
  }

  // Bind initialization tracking fields into persistent safe context references
  g_sntp_ctx.cid = cid;
  g_sntp_ctx.user_cb = cb;
  g_sntp_ctx.current_server_index = -1; // Force start configuration checkpoint
                                        // targeting primary argument string

  strncpy(g_sntp_ctx.primary_server_url, ntp_server,
          sizeof(g_sntp_ctx.primary_server_url) - 1);
  g_sntp_ctx.primary_server_url[sizeof(g_sntp_ctx.primary_server_url) - 1] =
      '\0';

  strncpy(g_sntp_ctx.target_tz, tz, sizeof(g_sntp_ctx.target_tz) - 1);
  g_sntp_ctx.target_tz[sizeof(g_sntp_ctx.target_tz) - 1] = '\0';

  // Execute the initial synchronization step
  nwy_hal_sntp_execute_step();
  return true;
}