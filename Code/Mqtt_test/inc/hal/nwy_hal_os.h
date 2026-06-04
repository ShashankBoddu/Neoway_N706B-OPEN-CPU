#ifndef __NWY_HAL_OS_H__
#define __NWY_HAL_OS_H__

#include <stdint.h>
#include <stdbool.h>
#include "nwy_osi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Thread APIs
typedef void (*nwy_hal_os_thread_func_t)(void *param);

bool nwy_hal_os_thread_create(nwy_osi_thread_t *thread_out, const char *name, 
                              nwy_hal_os_thread_func_t func, void *param, 
                              uint32_t priority, uint32_t stack_size);
void nwy_hal_os_thread_sleep(uint32_t ms);

// Timer APIs
typedef void (*nwy_hal_os_timer_cb_t)(void *ctx);

typedef enum {
    NWY_HAL_OS_TIMER_ONE_SHOT = 0,
    NWY_HAL_OS_TIMER_PERIODIC = 1
} nwy_hal_os_timer_type_e;

bool nwy_hal_os_timer_create(nwy_osi_timer_t *timer_out, nwy_hal_os_timer_cb_t cb, void *cb_para);
bool nwy_hal_os_timer_start(nwy_osi_timer_t timer, uint32_t interval_ms, nwy_hal_os_timer_type_e type);
bool nwy_hal_os_timer_stop(nwy_osi_timer_t timer);
bool nwy_hal_os_timer_delete(nwy_osi_timer_t timer);

// Semaphore APIs
bool nwy_hal_os_semaphore_create(nwy_osi_semaphore_t *sem_out, uint32_t init_count, uint32_t max_count);
bool nwy_hal_os_semaphore_acquire(nwy_osi_semaphore_t sem, uint32_t timeout_ms);
void nwy_hal_os_semaphore_release(nwy_osi_semaphore_t sem);
void nwy_hal_os_semaphore_delete(nwy_osi_semaphore_t sem);

// Mutex APIs
bool nwy_hal_os_mutex_create(nwy_osi_mutex_t *mutex_out);
bool nwy_hal_os_mutex_lock(nwy_osi_mutex_t mutex, uint32_t timeout_ms);
void nwy_hal_os_mutex_unlock(nwy_osi_mutex_t mutex);
void nwy_hal_os_mutex_delete(nwy_osi_mutex_t mutex);

#ifdef __cplusplus
}
#endif

#endif // __NWY_HAL_OS_H__
