#include "hal/net/nwy_hal_net.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/gpio/nwy_hal_gpio.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UART_PORT "URT1"
#define TEST_UART_BAUD 115200

#define TAG "SMSTest"
#define LOGI(fmt, ...) nwy_hal_uart_log_i(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) nwy_hal_uart_log_e(TAG, fmt, ##__VA_ARGS__)

static int g_uart_fd = -1;
#define printf(fmt, ...) nwy_hal_uart_printf(g_uart_fd, fmt, ##__VA_ARGS__)

#define TEST_PHONE_NUMBER "+918074638788"

#define MAX_PENDING_SMS 10
static volatile int g_pending_sms_indices[MAX_PENDING_SMS];
static volatile int g_pending_sms_count = 0;
static nwy_osi_mutex_t g_sms_mutex = NULL;

// Callback to handle incoming SMS - Just enqueues the index!
static void my_sms_recv_callback(int sim_id, int sms_index) {
  if (!g_sms_mutex) return;
  nwy_hal_os_mutex_lock(g_sms_mutex, 1000);
  if (g_pending_sms_count < MAX_PENDING_SMS) {
      g_pending_sms_indices[g_pending_sms_count++] = sms_index;
  } else {
      LOGE("Pending SMS queue is full! Dropping index %d", sms_index);
  }
  nwy_hal_os_mutex_unlock(g_sms_mutex);
}

// Process pending SMS from the main thread context
static void process_pending_sms(int sim_id) {
  if (!g_sms_mutex) return;
  nwy_hal_os_mutex_lock(g_sms_mutex, 1000);
  int count = g_pending_sms_count;
  int indices[MAX_PENDING_SMS];
  for (int i=0; i<count; i++) {
      indices[i] = g_pending_sms_indices[i];
  }
  g_pending_sms_count = 0;
  nwy_hal_os_mutex_unlock(g_sms_mutex);

  for (int i=0; i<count; i++) {
      int idx = indices[i];
      nwy_sms_recv_info_type_t sms_data;
      memset(&sms_data, 0, sizeof(sms_data));

      if (nwy_hal_sms_read(sim_id, idx, &sms_data)) {
          char phone_num[32];
          char message[161];
          char timestamp[32];

          strncpy(phone_num, sms_data.source_phone_num, sizeof(phone_num) - 1);
          phone_num[sizeof(phone_num) - 1] = '\0';
          
          const char *msg_ptr = strlen((char*)sms_data.msg_decoded_content) > 0 ? (char*)sms_data.msg_decoded_content : (char*)sms_data.msg_content;
          strncpy(message, msg_ptr, sizeof(message) - 1);
          message[sizeof(message) - 1] = '\0';

          snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                   sms_data.date.uYear, sms_data.date.uMonth, sms_data.date.uDay,
                   sms_data.date.uHour, sms_data.date.uMinute, sms_data.date.uSecond);

          LOGI("==================================================");
          LOGI("📨 INCOMING SMS PROCESSED!");
          LOGI("SIM Slot: %d", sim_id);
          LOGI("Sender:   %s", phone_num);
          LOGI("Time:     %s", timestamp);
          LOGI("Message:  %s", message);
          LOGI("==================================================");

          // Auto-reply/Echo functionality
          LOGI("Sending automatic echo reply...");
          char reply_msg[160];
          snprintf(reply_msg, sizeof(reply_msg), "Echo from Neoway OpenCPU! You said: %s", message);
          if (nwy_hal_sms_send(sim_id, phone_num, reply_msg)) {
            LOGI("Echo reply sent successfully to %s", phone_num);
          } else {
            LOGE("Failed to send echo reply.");
          }

          // Clean up the message from storage
          nwy_hal_sms_delete(sim_id, idx);
      } else {
          LOGE("Failed to read SMS index %d. It might have been deleted.", idx);
      }
  }
}

static void test_sms_storage(int sim_id) {
  LOGI("Scanning SIM Storage for existing SMS messages...");
  nwy_sms_msg_list_t sms_list;
  memset(&sms_list, 0, sizeof(sms_list));

  if (nwy_hal_sms_list_indices(sim_id, &sms_list)) {
    LOGI("Found %d messages in storage.", sms_list.len);
    for (uint16_t i = 0; i < sms_list.len; i++) {
      uint16_t idx = sms_list.indices[i];
      nwy_sms_recv_info_type_t sms_data;
      memset(&sms_data, 0, sizeof(sms_data));

      if (nwy_hal_sms_read(sim_id, idx, &sms_data)) {
        LOGI(" [Index %d] Sender: %s | Content: %s", idx,
             sms_data.source_phone_num,
             strlen((char *)sms_data.msg_decoded_content) > 0
                 ? (char *)sms_data.msg_decoded_content
                 : (char *)sms_data.msg_content);
      } else {
        LOGE(" [Index %d] Failed to read message.", idx);
      }
    }
  } else {
    LOGE("Failed to list SMS indices from storage.");
  }
}

static void sms_test_task(void *param) {
  nwy_hal_os_thread_sleep(3000);
  g_uart_fd = nwy_hal_uart_open(TEST_UART_PORT, TEST_UART_BAUD);
  if (g_uart_fd >= 0) {
    nwy_hal_uart_set_log_fd(g_uart_fd);
  }

  printf("\r\n==================================================\r\n");
  printf("   NEOWAY N706B OPENCPU - SMS API TEST SUITE  1    \r\n");
  printf("==================================================\r\n");

  int sim_id = 1;

  if (nwy_hal_os_mutex_create(&g_sms_mutex) == false) {
    LOGE("Failed to create SMS mutex.");
  }

  // 1. Wait for SIM and Network Registration
  LOGI("Waiting for SIM readiness...");
  nwy_sim_status_e sim_status;
  while (nwy_sim_status_get(NWY_SIM_ID_SLOT_1, &sim_status) != 0 ||
         sim_status != NWY_SIM_READY) {
    nwy_hal_os_thread_sleep(2000);
  }
  LOGI("SIM is READY.");

  LOGI("Waiting for CS network registration (SMS requires signaling plane)...");
  int cs_state = 0, ps_state = 0, rat = 0;
  while (1) {
    nwy_hal_net_get_registration_details(sim_id, &cs_state, &ps_state, &rat);
    if (cs_state == NWY_NW_SERVICE_FULL || cs_state == NWY_NW_SERVICE_LIMITED ||
        ps_state == NWY_NW_SERVICE_FULL) {
      break;
    }
    nwy_hal_os_thread_sleep(3000);
  }
  LOGI("Network is registered! (CS:%d, PS:%d, RAT:%d)", cs_state, ps_state,
       rat);

  // 2. Initialize SMS HAL
  LOGI("Initializing SMS Subsystem...");
  if (nwy_hal_sms_init(sim_id)) {
    LOGI("SMS Subsystem initialized successfully.");
  } else {
    LOGE("SMS Initialization FAILED.");
  }

  // 3. Register Receive Callback
  if (nwy_hal_sms_register_recv_cb(sim_id, my_sms_recv_callback)) {
    LOGI(
        "SMS URC Callback registered successfully. Ready to receive messages.");
  } else {
    LOGE("Failed to register SMS receive callback.");
  }

  // 4. Test Reading Storage
  test_sms_storage(sim_id);

  // 5. Send Boot-up SMS
  LOGI("Sending boot-up test SMS to %s...", TEST_PHONE_NUMBER);
  if (nwy_hal_sms_send(sim_id, TEST_PHONE_NUMBER,
                       "Hello from Neoway N706B OpenCPU SMS API!")) {
    LOGI("Boot-up SMS sent successfully!");
  } else {
    LOGE("Failed to send boot-up SMS.");
  }

  // Idle loop (awaiting incoming SMS URCs and processing them)
  while (1) {
    if (g_pending_sms_count > 0) {
        process_pending_sms(sim_id);
    }
    nwy_hal_os_thread_sleep(1000);
  }
}

// Background LED Blinker Thread
static void led_indicator_task(void *param) {
  // Simple LED Blinker for System Status
  nwy_hal_gpio_init_out(70, true); // Status LED 70
  nwy_hal_gpio_init_out(69, false); // Net LED 69

  while (1) {
    nwy_hal_gpio_toggle(69);
    nwy_hal_os_thread_sleep(1000);
  }
}

// Entry Point
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_hal_os_thread_sleep(1000);

  nwy_osi_thread_t led_thread = NULL;
  nwy_hal_os_thread_create(&led_thread, "led_indicator", led_indicator_task, NULL,
                           NWY_OSI_PRIORITY_NORMAL, 1024 * 2);

  nwy_osi_thread_t sms_thread = NULL;
  nwy_hal_os_thread_create(&sms_thread, "sms_test", sms_test_task, NULL,
                           NWY_OSI_PRIORITY_NORMAL, 1024 * 8);
  return 0;
}
