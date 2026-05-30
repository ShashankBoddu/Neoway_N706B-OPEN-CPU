#include "nwy_data_api.h"
#include "nwy_gpio_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_sim_api.h"
#include "hal/pm/nwy_hal_pm.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/net/nwy_hal_net.h"
#include "hal/sms/nwy_hal_sms.h"
#include "nwy_sms_api.h"
#include "nwy_vir_at_api.h"
#include "nwy_usb_serial.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPIO_NET_STATUS 69 // NET LED
#define GPIO_STATUS 70     // STATUS LED

volatile int g_net_status = 0;
// 0 = Not Registered / Searching
// 1 = Registered (Network OK)
// 2 = Data Call Connected (IP Acquired)

static nwy_osi_semaphore_t g_sms_sem = NULL;
static volatile bool s_pending_sms_reply = false;
static char s_pending_reply_phone[32];
static char s_pending_reply_msg[160];
static int s_pending_reply_sim = 1;

static void led_task(void *param) {
  nwy_gpio_direction_set(GPIO_NET_STATUS, PIN_DIRECTION_OUT);
  nwy_gpio_direction_set(GPIO_STATUS, PIN_DIRECTION_OUT);

  // STATUS LED solid ON to indicate module is running
  nwy_gpio_value_set(GPIO_STATUS, PIN_LEVEL_HIGH);

  int toggle = 0;
  while (1) {
    if (g_net_status == 0) {
      // Searching: Blink fast (200ms)
      toggle = !toggle;
      nwy_gpio_value_set(GPIO_NET_STATUS,
                         toggle ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
      nwy_thread_sleep(200);
    } else if (g_net_status == 1) {
      // Registered: Blink slow (1000ms)
      toggle = !toggle;
      nwy_gpio_value_set(GPIO_NET_STATUS,
                         toggle ? PIN_LEVEL_HIGH : PIN_LEVEL_LOW);
      nwy_thread_sleep(1000);
    } else if (g_net_status == 2) {
      // Data Connected: Solid ON
      nwy_gpio_value_set(GPIO_NET_STATUS, PIN_LEVEL_HIGH);
      nwy_thread_sleep(1000);
    }
  }
}

static void serial_log(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int len = vsprintf(buf, fmt, args);
  va_end(args);
  if (len > 0) {
    nwy_usb_serial_send(buf, len);
    nwy_usb_serial_send("\r\n", 2);
  }
}

static const char *get_reg_state_str(nwy_nw_service_type_e state) {
  switch (state) {
  case NWY_NW_SERVICE_NONE:
    return "OOS (No Service)";
  case NWY_NW_SERVICE_LIMITED:
    return "Limited Service";
  case NWY_NW_SERVICE_FULL:
    return "Full Service";
  default:
    return "Unknown";
  }
}

static const char *get_rat_str(int rat) {
  switch (rat) {
  case 1:  return "GSM";
  case 2:  return "WCDMA";
  case 3:  return "LTE (4G)";
  case 4:  return "CAT-M";
  case 5:  return "NB-IoT";
  case 6:  return "5G (NR)";
  default: return "None";
  }
}

static void my_net_callback(int sim_id, int profile_idx, bool connected, const char *ip_address) {
  if (connected) {
    serial_log("DATA CALLBACK: SIM %d (Profile %d) Connected! IP: %s", sim_id, profile_idx, ip_address ? ip_address : "Unknown");
    g_net_status = 2; // Data connected
  } else {
    serial_log("DATA CALLBACK: SIM %d (Profile %d) Disconnected!", sim_id, profile_idx);
    if (g_net_status == 2) {
      g_net_status = 1; // Drop back to registered state
    }
  }
}

static void my_sms_recv_callback(int sim_id, const char *phone_num, const char *message, const char *timestamp) {
  serial_log("SMS RECEIVED on SIM %d from %s at %s: %s", sim_id, phone_num, timestamp, message);
  
  if (!s_pending_sms_reply) {
      s_pending_reply_sim = sim_id;
      strncpy(s_pending_reply_phone, phone_num, sizeof(s_pending_reply_phone) - 1);
      s_pending_reply_phone[sizeof(s_pending_reply_phone) - 1] = '\0';
      
      strncpy(s_pending_reply_msg, message, sizeof(s_pending_reply_msg) - 1);
      s_pending_reply_msg[sizeof(s_pending_reply_msg) - 1] = '\0';
      
      s_pending_sms_reply = true;
      if (g_sms_sem) {
          nwy_semahpore_release(g_sms_sem);
      }
  }
}

static void network_monitor_task(void *param) {
  nwy_thread_sleep(5000); // Wait for module to initialize

  serial_log("--- Network Monitor Started (HAL Version) ---");
  serial_log("Boot Reason: %s", nwy_hal_pm_get_boot_reason_str());

  static bool s_data_call_started = false;
  static bool s_sms_initialized = false;

  while (1) {
    // 0. Process any pending SMS echo replies in task context
    if (s_pending_sms_reply) {
        char reply_body[160];
        snprintf(reply_body, sizeof(reply_body), "Echo: %s", s_pending_reply_msg);
        
        serial_log("Processing pending SMS reply in task context...");
        serial_log("Sending reply on SIM %d to %s...", s_pending_reply_sim, s_pending_reply_phone);
        
        if (nwy_hal_sms_send(s_pending_reply_sim, s_pending_reply_phone, reply_body)) {
            serial_log("SMS reply sent successfully!");
        } else {
            serial_log("Failed to send SMS reply!");
        }
        s_pending_sms_reply = false;
    }

    // 1. Check SIM Status
    if (nwy_hal_sim_is_ready(1)) {
      serial_log("SIM Status: READY");
      
      // Initialize SMS if SIM ready
      if (!s_sms_initialized) {
          serial_log("Initializing SMS module...");
          if (nwy_hal_sms_init(1)) {
              if (nwy_hal_sms_register_recv_cb(1, my_sms_recv_callback)) {
                  serial_log("SMS module initialized successfully!");
                  s_sms_initialized = true;
              } else {
                  serial_log("Failed to register SMS receive callback!");
              }
          } else {
              serial_log("Failed to initialize SMS module!");
          }
      }
      
      char imsi[32] = {0};
      char iccid[32] = {0};
      if (nwy_hal_sim_get_imsi(1, imsi)) {
          serial_log("IMSI: %s", imsi);
      }
      if (nwy_hal_sim_get_iccid(1, iccid)) {
          serial_log("ICCID: %s", iccid);
      }

      // 2. Check Network Registration
      int cs_state = 0, ps_state = 0, rat = 0;
      if (nwy_hal_net_get_registration_details(1, &cs_state, &ps_state, &rat)) {
          serial_log("CS Reg: %s, RAT: %s", get_reg_state_str(cs_state), get_rat_str(rat));
          serial_log("PS Reg: %s, RAT: %s", get_reg_state_str(ps_state), get_rat_str(rat));
          
          if (ps_state == 2 || cs_state == 2) {
              g_net_status = 1;
          } else {
              g_net_status = 0;
          }
      } else {
          serial_log("CS Reg: Unknown, RAT: None");
          serial_log("PS Reg: Unknown, RAT: None");
          g_net_status = 0;
      }

      // 3. Handle Data Call and IP Query
      char ip_address[32] = {0};
      if (g_net_status > 0) {
          if (nwy_hal_net_get_ip(1, 1, ip_address, sizeof(ip_address))) {
              g_net_status = 2; // Data Call Connected
              serial_log("Data Call Connected! IP: %s", ip_address);
          } else {
              if (g_net_status == 2) {
                  g_net_status = 1; // Drop back to registered state if we lost IP
              }
              if (!s_data_call_started) {
                  serial_log("Data Call Not Connected. Starting Call...");
                  nwy_hal_net_start_data_call(1, 1, my_net_callback);
                  s_data_call_started = true;
              } else {
                  serial_log("Data Call Connecting/Idle...");
              }
          }
      } else {
          s_data_call_started = false;
      }

      // 3.5. Send boot-up test SMS once connected
      static bool s_boot_sms_sent = false;
      if (g_net_status == 2 && !s_boot_sms_sent) {
          serial_log("Sending boot-up test SMS to +918074638788...");
          if (nwy_hal_sms_send(1, "+918074638788", "Neoway N706B Network Monitor: System successfully booted and online!")) {
              serial_log("Boot-up SMS sent successfully!");
              s_boot_sms_sent = true;
          } else {
              serial_log("Failed to send boot-up SMS!");
          }
      }

      // 4. Check Signal Strength and Operator Info
      int csq = 99, ber = 99;
      if (nwy_hal_net_get_csq(1, &csq, &ber)) {
          serial_log("Signal (CSQ): %d, BER: %d", csq, ber);
      }
      
      char op_name[64] = {0};
      if (nwy_hal_net_get_operator_name(1, op_name, sizeof(op_name))) {
          serial_log("Operator: %s", op_name);
      }
    } else {
      g_net_status = 0;
      serial_log("SIM Status: NOT READY");
    }

    serial_log("-------------------------------");
    if (g_sms_sem) {
        nwy_semaphore_acquire(g_sms_sem, 10000); // Wake up on sem release OR 10-second timeout
    } else {
        nwy_thread_sleep(10000);
    }
  }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry()
#else
int appimg_enter(void *param)
#endif
{
  nwy_thread_sleep(10 * 1000); // wait for PC to enumerate USB serial
  serial_log("Network Monitor App Entered");

  // Initialize the SMS wakeup semaphore
  nwy_semaphore_create(&g_sms_sem, 0);

  nwy_osi_thread_t net_thread = NULL;
  nwy_error_e ret =
      nwy_thread_create(&net_thread, "net_monitor", NWY_OSI_PRIORITY_NORMAL,
                        network_monitor_task, NULL, 10, 1024 * 4, NULL);
  if (ret != NWY_SUCCESS) {
    serial_log("Failed to create network thread! Error: %d", ret);
  } else {
    serial_log("Network thread created successfully.");
  }

  nwy_osi_thread_t led_th_hdl = NULL;
  nwy_thread_create(&led_th_hdl, "led_task", NWY_OSI_PRIORITY_NORMAL, led_task,
                    NULL, 10, 1024 * 2, NULL);

  return 0;
}

void appimg_exit(void) { serial_log("Network Monitor App Exited"); }
