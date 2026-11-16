#include "hal/os/nwy_hal_os.h"
#include <string.h>

bool nwy_hal_os_thread_create(nwy_osi_thread_t *thread_out, const char *name, 
                              nwy_hal_os_thread_func_t func, void *param, 
                              uint32_t priority, uint32_t stack_size) {
    if (!thread_out || !func) return false;
    
    // Convert generic thread function type to SDK thread entry pointer (nwy_osi_cb_func)
    nwy_error_e ret = nwy_thread_create(thread_out, (char *)name, (uint8)priority, 
                                        (nwy_osi_cb_func)func, param, 10, stack_size, NULL);
    return (ret == NWY_SUCCESS);
}

void nwy_hal_os_thread_sleep(uint32_t ms) {
    nwy_thread_sleep(ms);
}

bool nwy_hal_os_timer_create(nwy_osi_timer_t *timer_out, nwy_hal_os_timer_cb_t cb, void *cb_para) {
    if (!timer_out || !cb) return false;
    
    nwy_timer_para_t timer_para;
    memset(&timer_para, 0, sizeof(timer_para));
    timer_para.expired_time = 0; // Will be set on start
    timer_para.type = NWY_TIMER_ONE_TIME;
    timer_para.thread_hdl = NWY_TIMER_IN_SERVICE; // Runs in system thread context
    timer_para.cb = cb;
    timer_para.cb_para = cb_para;
    
    nwy_error_e ret = nwy_sdk_timer_create(timer_out, &timer_para);
    return (ret == NWY_SUCCESS);
}

bool nwy_hal_os_timer_start(nwy_osi_timer_t timer, uint32_t interval_ms, nwy_hal_os_timer_type_e type) {
    if (!timer) return false;
    
    nwy_timer_para_t timer_para;
    memset(&timer_para, 0, sizeof(timer_para));
    timer_para.expired_time = interval_ms;
    timer_para.type = (type == NWY_HAL_OS_TIMER_PERIODIC) ? NWY_TIMER_PERIODIC : NWY_TIMER_ONE_TIME;
    timer_para.thread_hdl = NWY_TIMER_IN_SERVICE;
    
    nwy_error_e ret = nwy_sdk_timer_start(timer, &timer_para);
    return (ret == NWY_SUCCESS);
}

bool nwy_hal_os_timer_stop(nwy_osi_timer_t timer) {
    if (!timer) return false;
    return (nwy_sdk_timer_stop(timer) == NWY_SUCCESS);
}

bool nwy_hal_os_timer_delete(nwy_osi_timer_t timer) {
    if (!timer) return false;
    return (nwy_sdk_timer_destory(timer) == NWY_SUCCESS);
}

bool nwy_hal_os_semaphore_create(nwy_osi_semaphore_t *sem_out, uint32_t init_count, uint32_t max_count) {
    if (!sem_out) return false;
    nwy_error_e ret = nwy_semaphore_create(sem_out, max_count);
    return (ret == NWY_SUCCESS);
}

bool nwy_hal_os_semaphore_acquire(nwy_osi_semaphore_t sem, uint32_t timeout_ms) {
    if (!sem) return false;
    
    int timeout = (timeout_ms == 0xFFFFFFFF) ? -1 : (int)timeout_ms;
    nwy_error_e ret = nwy_semaphore_acquire(sem, timeout);
    return (ret == NWY_SUCCESS);
}

void nwy_hal_os_semaphore_release(nwy_osi_semaphore_t sem) {
    if (sem) {
        nwy_semahpore_release(sem);
    }
}

void nwy_hal_os_semaphore_delete(nwy_osi_semaphore_t sem) {
    if (sem) {
        nwy_semahpore_delete(sem);
    }
}

bool nwy_hal_os_mutex_create(nwy_osi_mutex_t *mutex_out) {
    if (!mutex_out) return false;
    nwy_error_e ret = nwy_sdk_mutex_create(mutex_out);
    return (ret == NWY_SUCCESS);
}

bool nwy_hal_os_mutex_lock(nwy_osi_mutex_t mutex, uint32_t timeout_ms) {
    if (!mutex) return false;
    
    int timeout = (timeout_ms == 0xFFFFFFFF) ? -1 : (int)timeout_ms;
    nwy_error_e ret = nwy_sdk_mutex_lock(mutex, timeout);
    return (ret == NWY_SUCCESS);
}

void nwy_hal_os_mutex_unlock(nwy_osi_mutex_t mutex) {
    if (mutex) {
        nwy_sdk_mutex_unlock(mutex);
    }
}

void nwy_hal_os_mutex_delete(nwy_osi_mutex_t mutex) {
    if (mutex) {
        nwy_sdk_mutex_delete(mutex);
    }
}
