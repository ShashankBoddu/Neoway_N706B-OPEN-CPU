#include <stdlib.h>
#include <string.h>
#include "nwy_osi_api.h"
#include "nwy_log_api.h"
#include "nwy_uart_api.h"

#define ECHO_UART_NAME "URT2"
#define ECHO_UART_BAUD 115200

static int uart_fd = -1;

// UART Receive Callback
static void nwy_uart_echo_recv_task(const char *str, uint32_t length)
{
    if (uart_fd >= 0 && length > 0) {
        // Echo the received data back
        nwy_uart_write(uart_fd, (const unsigned char *)str, length);
        
        // Also log it for debugging
        NWY_SDK_LOG_DEBUG("UART Echo: received %d bytes", length);
    }
}

static void uart_echo_init_task(void *param)
{
    nwy_thread_sleep(2000); // Wait for system stability
    
    NWY_SDK_LOG_DEBUG("Initializing UART Echo on %s...", ECHO_UART_NAME);
    
    uart_fd = nwy_uart_open(ECHO_UART_NAME, ECHO_UART_BAUD, FC_NONE);
    if (uart_fd < 0) {
        NWY_SDK_LOG_DEBUG("Failed to open UART %s, error: %d", ECHO_UART_NAME, uart_fd);
        return;
    }
    
    // Register receive callback
    nwy_uart_rx_cb_register(uart_fd, nwy_uart_echo_recv_task);
    
    NWY_SDK_LOG_DEBUG("UART Echo initialized successfully at %d baud", ECHO_UART_BAUD);
    
    char *msg = "\r\n--- Neoway N706B UART Echo Started ---\r\n";
    nwy_uart_write(uart_fd, (const unsigned char *)msg, strlen(msg));
    
    while(1) {
        nwy_thread_sleep(5000);
        NWY_SDK_LOG_DEBUG("UART Echo Task Heartbeat...");
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
    nwy_thread_sleep(1000);
    NWY_SDK_LOG_DEBUG("UART Echo Application Entered...");
    
    nwy_osi_thread_t uart_thread = NULL;
    nwy_thread_create(&uart_thread, "uart_echo", NWY_OSI_PRIORITY_NORMAL, uart_echo_init_task, NULL, 0, 1024 * 4, NULL);
    
    return 0;
}

void appimg_exit(void)
{
    NWY_SDK_LOG_DEBUG("UART Echo Application Exited");
    if (uart_fd >= 0) {
        nwy_uart_close(uart_fd);
    }
}
