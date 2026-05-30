#include "nwy_dm_api.h"
#include "nwy_gpio_api.h"
#include "nwy_log_api.h"
#include "nwy_osi_api.h"
#include "nwy_usb_serial.h"
#include <stdlib.h>

#include <string.h>

#define INDICATOR_GPIO_ID 70

nwy_osi_thread_t hello_world_thread = NULL;

static int hello_world_usb_recv_cb(void *data, size_t size) {
  // Echo back whatever we receive! This is a great bidirectional check on
  // COM24.
  if (size > 0 && data != NULL) {
    nwy_usb_serial_send(data, size);
  }
  return size;
}

static void hello_world_func(void *param) {
  int dir_ret = nwy_gpio_direction_set(INDICATOR_GPIO_ID, PIN_DIRECTION_OUT);

  int counter = 0;
  int state = 0;
  while (1) {
    // Toggle GPIO to prove the thread is running on the hardware
    state = !state;
    int val_ret = nwy_gpio_value_set(INDICATOR_GPIO_ID,
                                     state ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);

    // 1. Print to USB DIAG port (binary log)
    NWY_SDK_LOG_DEBUG("->> Hello World from Neoway N706B OpenCPU! Count: %d, "
                      "GPIO: %d, dir_ret: %d, val_ret: %d",
                      counter, state, dir_ret, val_ret);

    // 2. Print directly to USB Serial (COM24 / OPENCON port) as raw text
    char msg[192];
    int len = sprintf(msg,
                      "Hello World from Neoway N706B OpenCPU via COM24! Count: "
                      "%d, GPIO: %d, dir_ret: %d, val_ret: %d\r\n",
                      counter, state, dir_ret, val_ret);
    if (len > 0) {
      nwy_usb_serial_send(msg, len);
    }

    counter++;
    nwy_thread_sleep(1000);
  }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  // Sleep for 10 seconds to let the system USB stack stabilize on the PC
  nwy_thread_sleep(10 * 1000);

  // Set APP version so AT+NAPPCHECK? command on COM25 (AT port) returns it
  char version[] = "\"HelloWorld_V1.0.0\"";
  nwy_dm_app_version_set(version, strlen(version));

  // Register the USB Serial callback to activate the COM24 endpoint
  nwy_usb_serial_reg_recv_cb(hello_world_usb_recv_cb);

  NWY_SDK_LOG_DEBUG("Application Entered...");

  nwy_error_e ret = nwy_thread_create(&hello_world_thread, "hello_world",
                                      NWY_OSI_PRIORITY_NORMAL, hello_world_func,
                                      NULL, 10, 1024 * 4, NULL);
  if (ret != NWY_SUCCESS) {
    NWY_SDK_LOG_ERROR("Failed to create hello_world thread, error: %d", ret);
  } else {
    NWY_SDK_LOG_DEBUG("hello_world thread created successfully!");
  }

  return 0;
}

void appimg_exit(void) { NWY_SDK_LOG_DEBUG("Application Exited"); }
