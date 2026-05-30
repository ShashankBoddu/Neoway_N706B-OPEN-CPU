#include "nwy_data_api.h"
#include "nwy_gpio_api.h"
#include "nwy_log_api.h"
#include "nwy_network_api.h"
#include "nwy_osi_api.h"
#include "nwy_sim_api.h"
#include "hal/pm/nwy_hal_pm.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/net/nwy_hal_net.h"
#include "nwy_sms_api.h"
#include "nwy_vir_at_api.h"
#include "nwy_usb_serial.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>

#define GPIO_NET_STATUS 69 // NET LED
#define GPIO_STATUS 70     // STATUS LED

volatile int g_net_status = 0;
// 0 = Not Registered / Searching
// 1 = Registered (Network OK)
// 2 = Data Call Connected (IP Acquired)

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

static void my_net_callback(bool connected, const char *ip_address) {
  if (connected) {
    serial_log("DATA CALLBACK: Connected! IP: %s", ip_address ? ip_address : "Unknown");
    g_net_status = 2; // Data connected
  } else {
    serial_log("DATA CALLBACK: Disconnected!");
    if (g_net_status == 2) {
      g_net_status = 1; // Drop back to registered state
    }
  }
}

static void network_monitor_task(void *param) {
  nwy_thread_sleep(5000); // Wait for module to initialize

  serial_log("--- Network Monitor Started (HAL Version) ---");
  serial_log("Boot Reason: %s", nwy_hal_pm_get_boot_reason_str());

  while (1) {
    // 1. Check SIM Status
    if (nwy_hal_sim_is_ready(1)) {
      serial_log("SIM Status: READY");
      
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
          if (nwy_hal_net_get_ip(1, ip_address, sizeof(ip_address))) {
              g_net_status = 2; // Data Call Connected
              serial_log("Data Call Connected! IP: %s", ip_address);
          } else {
              // Try starting data call
              serial_log("Data Call Not Connected. Starting Call...");
              nwy_hal_net_start_data_call(1, my_net_callback);
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
    nwy_thread_sleep(10000); // Check every 10 seconds
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
