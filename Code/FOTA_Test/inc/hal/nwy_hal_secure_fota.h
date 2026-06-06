#ifndef __NWY_HAL_SECURE_FOTA_H__
#define __NWY_HAL_SECURE_FOTA_H__

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

bool nwy_hal_secure_fota_start(const char *url, uint32_t file_size,
                               const char *signature_hex);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_SECURE_FOTA_H__