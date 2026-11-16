#ifndef __NWY_HAL_SSL_H__
#define __NWY_HAL_SSL_H__

#include "nwy_ssl_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocates and configures a secure TLS v1.2 context profile slot.
 * @param ssl_ctx_id Unique slot tracker index (0 to 5).
 * @param ca_cert_pem Certificate chain block (Set to NULL to skip peer checks).
 * @param host_name Target domain string required to populate Server Name
 * Indication (SNI).
 */
nwy_ssl_conf_t *nwy_hal_ssl_create_context(int ssl_ctx_id,
                                           const char *ca_cert_pem,
                                           const char *host_name);

void nwy_hal_ssl_destroy_context(int ssl_ctx_id);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SSL_H__