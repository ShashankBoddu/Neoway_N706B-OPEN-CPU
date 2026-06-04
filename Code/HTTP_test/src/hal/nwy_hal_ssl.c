#include "hal/nwy_hal_ssl.h"
#include "hal/nwy_hal_uart.h"
#include <stdlib.h>
#include <string.h>

#define TAG "HAL_SSL"

static nwy_ssl_conf_t g_ssl_config_slots[6];
static bool g_slot_active[6] = {false};

nwy_ssl_conf_t *nwy_hal_ssl_create_context(int ssl_ctx_id,
                                           const char *ca_cert_pem,
                                           const char *host_name) {
  if (ssl_ctx_id < 0 || ssl_ctx_id > 5) {
    nwy_hal_uart_log_e(TAG, "Invalid SSL context slot index: %d", ssl_ctx_id);
    return NULL;
  }

  nwy_ssl_conf_t *cfg = &g_ssl_config_slots[ssl_ctx_id];
  memset(cfg, 0, sizeof(nwy_ssl_conf_t));

  // Lock configuration parameters to modern TLS protocol rules
  cfg->ssl_version = NWY_VERSION_TLS_V1_3_E; // Upgrade to TLS 1.3 for modern CDNs like Netlify

  // EXTENSION FIX: Dynamically populate SNI fields to bypass multi-domain
  // gateway blocks
  if (host_name != NULL && strlen(host_name) > 0) {
    cfg->sni_name = (char *)host_name;
    cfg->sni_name_size = strlen(host_name) + 1; // Include null terminator for SNI length
  }

  if (ca_cert_pem != NULL) {
    nwy_hal_uart_log_i(TAG, "Loading validation chain into slot [%d]...",
                       ssl_ctx_id);
    cfg->authmode = NWY_SSL_AUTH_ONE_WAY_E;
  } else {
    nwy_hal_uart_log_w(
        TAG, "CA Cert is NULL. Encryption active with verification bypassed.");
    cfg->authmode = NWY_SSL_AUTH_NONE_E;
  }

  g_slot_active[ssl_ctx_id] = true;
  return cfg;
}

void nwy_hal_ssl_destroy_context(int ssl_ctx_id) {
  if (ssl_ctx_id >= 0 && ssl_ctx_id <= 5 && g_slot_active[ssl_ctx_id]) {
    g_slot_active[ssl_ctx_id] = false;
    nwy_hal_uart_log_i(TAG, "SSL context slot [%d] cleared from memory.",
                       ssl_ctx_id);
  }
}