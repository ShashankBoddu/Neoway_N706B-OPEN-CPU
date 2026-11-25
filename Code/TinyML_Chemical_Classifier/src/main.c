/**
 * @file main.c
 * @brief Neoway N706B OpenCPU — BME688 AI Gas Scanner & SMS-Controlled Data Collector
 * 
 * Target Platform: Neoway N706B-CN / N706B-CN-10 (ASR1605S Crane ARM Cortex-R5 @ 614 MHz)
 * Sensor: SmartElex Environmental Sensor BME688 on I2C1 (Pin 57 SCL, Pin 58 SDA)
 * Storage: MicroSD Card via SPI (SSP0) using FatFs (Pin 83 SD Power, Pin 79 CS)
 * Telemetry: 3GPP SMS Event-Driven Wakeup & Confirmation Dispatch
 */

#include "bme68x.h"
#include "i2c_bus.h"
#include "nwy_sd_card.h"
#include "data_collector.h"
#include "remote_sd_manager.h"
#include "mqtt_manager.h"
#include "hal/nwy_hal_secure_fota.h"
#include "hal/net/nwy_hal_net.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/pm/nwy_hal_pm.h"
#include "nwy_gpio_api.h"
#include "nwy_uart_api.h"
#include "nwy_pm_api.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define DEBUG_UART_PORT         "URT1"
#define DEBUG_UART_BAUD         115200

#define PIN_PERIPHERAL_PWR_GPIO0 0       /* Module Pin 83 (GPIO_00: Q1/Q3 & Q5/Q10 Master Gate) */
#define BME688_I2C_ADDR         0x76    /* Default SmartElex BME688 address (SDO tied to GND) */

#define CodeVer "0.1"
/* Global Device Handles */
static bme68x_dev_t        g_bme_dev;
static uint8_t             g_bme_addr = BME688_I2C_ADDR;
static int                 g_dbg_uart_fd = -1;

/* FreeRTOS Tasks and Synchronization */
#define SMS_QUEUE_CAPACITY 16
static nwy_osi_semaphore_t g_sms_sem = NULL;
static nwy_osi_mutex_t     g_sms_q_mutex = NULL;
static int                 g_sms_queue[SMS_QUEUE_CAPACITY];
static volatile int        g_sms_q_head = 0;
static volatile int        g_sms_q_tail = 0;
static nwy_osi_thread_t    g_collector_thread = NULL;
static nwy_osi_thread_t    g_sms_worker_thread = NULL;

void sd_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_dbg_uart_fd >= 0) {
        nwy_hal_uart_printf(g_dbg_uart_fd, "%s\r\n", buf);
    }
}

/* -------------------------------------------------------------------------
 * BME688 LOW-LEVEL I2C CALLBACKS
 * ------------------------------------------------------------------------- */
static int8_t bme_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    uint8_t dev_addr = *(uint8_t *)intf_ptr;
    int ret = i2c_bus_read_reg(dev_addr, reg_addr, reg_data, (uint16_t)len);
    return (ret == 0) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static int8_t bme_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    uint8_t dev_addr = *(uint8_t *)intf_ptr;
    int ret = i2c_bus_write_reg(dev_addr, reg_addr, reg_data, (uint16_t)len);
    return (ret == 0) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static void bme_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us < 1000) {
        nwy_thread_usleep((int)period_us);
    } else {
        nwy_hal_os_thread_sleep(period_us / 1000);
    }
}

/* -------------------------------------------------------------------------
 * INCOMING SMS NOTIFICATION CALLBACK
 * ------------------------------------------------------------------------- */
static void on_incoming_sms_cb(int sim_id, int sms_index)
{
    (void)sim_id;
    if (g_sms_q_mutex) nwy_sdk_mutex_lock(g_sms_q_mutex, 100);
    int next_head = (g_sms_q_head + 1) % SMS_QUEUE_CAPACITY;
    if (next_head != g_sms_q_tail) {
        g_sms_queue[g_sms_q_head] = sms_index;
        g_sms_q_head = next_head;
    } else {
        if (g_dbg_uart_fd >= 0) {
            nwy_hal_uart_printf(g_dbg_uart_fd, "[SMS Q full: %d]\r\n", sms_index);
        }
    }
    if (g_sms_q_mutex) nwy_sdk_mutex_unlock(g_sms_q_mutex);

    if (g_sms_sem) {
        nwy_hal_os_semaphore_release(g_sms_sem);
    }
}

/* -------------------------------------------------------------------------
 * SMS WORKER THREAD
 * ------------------------------------------------------------------------- */
static void sms_worker_task(void *param)
{
    (void)param;
    nwy_hal_uart_printf(g_dbg_uart_fd, "[SMS Worker active]\r\n");

    while (1) {
        /* Block indefinitely until an incoming SMS releases g_sms_sem */
        nwy_hal_os_semaphore_acquire(g_sms_sem, 0xFFFFFFFF);

        int idx = -1;
        if (g_sms_q_mutex) nwy_sdk_mutex_lock(g_sms_q_mutex, 100);
        if (g_sms_q_head != g_sms_q_tail) {
            idx = g_sms_queue[g_sms_q_tail];
            g_sms_q_tail = (g_sms_q_tail + 1) % SMS_QUEUE_CAPACITY;
        }
        if (g_sms_q_mutex) nwy_sdk_mutex_unlock(g_sms_q_mutex);

        if (idx >= 0) {
            nwy_sms_recv_info_type_t sms_data;
            memset(&sms_data, 0, sizeof(sms_data));

            if (nwy_hal_sms_read(1, idx, &sms_data)) {
                nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n");
                nwy_hal_uart_printf(g_dbg_uart_fd, "[SMS CMD IN]\r\n");
                nwy_hal_uart_printf(g_dbg_uart_fd, "  * From Phone : %s\r\n", sms_data.source_phone_num);
                nwy_hal_uart_printf(g_dbg_uart_fd, "  * Command    : \"%s\"\r\n", sms_data.msg_content);
                nwy_hal_uart_printf(g_dbg_uart_fd, "");

                /* Parse and execute command (START, STOP, STATUS, INTERVAL, HELP) */
                data_collector_process_sms(sms_data.source_phone_num, (const char *)sms_data.msg_content);

                /* Delete SMS from SIM storage to keep slots available */
                nwy_hal_sms_delete(1, idx);
            } else {
                nwy_hal_uart_printf(g_dbg_uart_fd, "[SMS read err %d]\r\n", idx);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * SYSTEM BOOT BANNER
 * ------------------------------------------------------------------------- */
static void print_system_banner(void)
{
    nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n[N706B v%s (%s)]\r\n", CodeVer, __DATE__);
}

/* -------------------------------------------------------------------------
 * MAIN APPLICATION TASK
 * ------------------------------------------------------------------------- */
static void app_main_task(void *param)
{
    (void)param;

    /* 1. Open Diagnostic Console UART (URT1 @ 115200) */
    g_dbg_uart_fd = nwy_hal_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD);
    if (g_dbg_uart_fd >= 0) {
        nwy_hal_uart_set_log_fd(g_dbg_uart_fd);
    }

    print_system_banner();

    /* 2. Assert Master Peripheral Rail Switch: GPIO 00 = HIGH */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[1/6] Rails ON\r\n");
    nwy_gpio_direction_set(PIN_PERIPHERAL_PWR_GPIO0, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(PIN_PERIPHERAL_PWR_GPIO0, PIN_LEVEL_HIGH);
    nwy_hal_os_thread_sleep(100); /* Allow sensor LDO & MicroSD rails to settle */

    /* 3. Initialize I2C Bus 1 Subsystem */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[2/6] I2C1 Init\r\n");
    i2c_bus_config_t i2c_cfg;
    i2c_cfg.bus_name   = I2C1_BUS_NAME;
    i2c_cfg.speed_hz   = I2C_DEFAULT_SPEED_HZ;
    i2c_cfg.scl_gpio   = I2C1_DEFAULT_SCL_GPIO;
    i2c_cfg.sda_gpio   = I2C1_DEFAULT_SDA_GPIO;
    i2c_cfg.mode       = I2C_MODE_AUTO;
    i2c_cfg.timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    i2c_bus_init(&i2c_cfg);

    /* 4. Detect and Initialize Bosch BME688 Sensor */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[3/6] Probe BME688\r\n", g_bme_addr);
    bool ack = i2c_bus_ping(g_bme_addr);
    if (!ack) {
        /* Probe alternate address 0x77 */
        nwy_hal_uart_printf(g_dbg_uart_fd, "  0x%02X NACK, probe 0x77\r\n", g_bme_addr);
        if (i2c_bus_ping(0x77)) {
            g_bme_addr = 0x77;
            ack = true;
        }
    }

    if (ack) {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  ACK 0x%02X\r\n", g_bme_addr);
        memset(&g_bme_dev, 0, sizeof(g_bme_dev));
        g_bme_dev.dev_addr = g_bme_addr;
        g_bme_dev.intf_ptr = &g_bme_addr;
        g_bme_dev.read     = bme_i2c_read;
        g_bme_dev.write    = bme_i2c_write;
        g_bme_dev.delay_us = bme_delay_us;
        g_bme_dev.amb_temp = 25.0f;

        /* Test direct Chip ID read before init */
        uint8_t pre_id = 0;
        int pre_ret = bme_i2c_read(BME68X_REG_CHIP_ID, &pre_id, 1, &g_bme_addr);
        nwy_hal_uart_printf(g_dbg_uart_fd, "  Chip ID: 0x%02X\r\n", pre_id, pre_ret);

        int8_t rslt = bme68x_init(&g_bme_dev);
        if (rslt != BME68X_OK) {
            /* Retry init with bus reset in case of power-up transient */
            for (int r = 1; r <= 3; r++) {
                nwy_hal_uart_printf(g_dbg_uart_fd, "  Retry #%d bme68x_init\r\n", r);
                i2c_bus_reset();
                nwy_hal_os_thread_sleep(50);
                rslt = bme68x_init(&g_bme_dev);
                if (rslt == BME68X_OK) break;
            }
        }

        if (rslt == BME68X_OK && g_bme_dev.chip_id == BME68X_CHIP_ID) {
            nwy_hal_uart_printf(g_dbg_uart_fd, "  [BME688 READY]\r\n");
            nwy_hal_uart_printf(g_dbg_uart_fd, "  Variant: %s\r\n",
                                g_bme_dev.chip_id,
                                (g_bme_dev.variant_id == BME68X_VARIANT_GAS_HIGH) ? "BME688 (Gas High)" : "BME680 (Gas Low)");
        } else {
            nwy_hal_uart_printf(g_dbg_uart_fd, "  ERR: init %d (0x%02X)\r\n", rslt, g_bme_dev.chip_id);
            nwy_hal_uart_printf(g_dbg_uart_fd, "");
        }
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  ERR: No I2C ACK\r\n");
    }

    /* 5. Initialize MicroSD Card SPI & FatFs Engine */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[4/6] MicroSD Init\r\n");
    int sd_ret = nwy_sdcard_init(NULL);
    if (sd_ret == 0) {
        uint64_t free_bytes = nwy_sdcard_get_free_space();
        uint64_t total_bytes = nwy_sdcard_get_total_space();
        nwy_hal_uart_printf(g_dbg_uart_fd, "  SD: %luMB free\r\n",
                            (unsigned long)(total_bytes / (1024 * 1024)),
                            (unsigned long)(free_bytes / (1024 * 1024)));
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  WARN: SD not mounted (%d)\r\n", sd_ret);
    }

    /* 6. Initialize Data Collector State Machine */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[5/6] Collector Init\r\n");
    data_collector_init(&g_bme_dev);

    /* 7. Initialize SIM & Arm 3GPP SMS Engine */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[6/6] SMS Init\r\n");
    nwy_sdk_mutex_create(&g_sms_q_mutex);
    nwy_hal_os_semaphore_create(&g_sms_sem, 0, SMS_QUEUE_CAPACITY);

    int sim_wait = 0;
    while (!nwy_hal_sim_is_ready(1) && sim_wait < 15) {
        nwy_hal_os_thread_sleep(1000);
        sim_wait++;
    }

    bool bme_ok = (g_bme_dev.chip_id == BME68X_CHIP_ID);
    bool sd_ok = nwy_sdcard_is_mounted();

    /* 8. Initialize Cellular LTE Mode & Remote SD Uploader Engine */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[Remote SD Init]\r\n");
    nwy_hal_net_set_mode(1, NWY_NW_MODE_MASK_LTE);
    remote_sd_manager_init();

    if (nwy_hal_sim_is_ready(1)) {
        char phone_num[32] = {0};
        nwy_hal_sim_get_msisdn(1, phone_num, sizeof(phone_num));
        nwy_hal_uart_printf(g_dbg_uart_fd, "  SIM ready: %s\r\n",
                            phone_num[0] ? phone_num : "Unknown");

        nwy_hal_sms_init(1, NWY_SMS_STORAGE_TYPE_UIM);
        nwy_hal_sms_register_recv_cb(1, on_incoming_sms_cb);
        nwy_hal_uart_printf(g_dbg_uart_fd, "  SMS ready\r\n");

        /* Start LTE Data Call for Remote SD HTTP Uploads, FOTA & MQTT */
        nwy_hal_uart_printf(g_dbg_uart_fd, "  Data call CID1\r\n");
        nwy_hal_net_start_data_call(1, 1, NULL);

        /* Initialize Paho MQTT client & auto-reconnect engine */
        nwy_hal_uart_printf(g_dbg_uart_fd, "  MQTT Init\r\n");
        mqtt_manager_init();

        /* Send one-time boot notification SMS to configured alert phone (+918074638788) */
        data_collector_send_boot_status(bme_ok, sd_ok, CodeVer);
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  WARN: SIM timeout %ds\r\n", sim_wait);
    }

    /* 9. Launch Background Worker Threads */
    nwy_hal_uart_printf(g_dbg_uart_fd, "Workers started\r\n");
    nwy_thread_create(&g_collector_thread, "data_collector", 50,
                      data_collector_worker_task, NULL, 10, 1024 * 16, NULL);

    nwy_thread_create(&g_sms_worker_thread, "sms_worker", 45,
                      sms_worker_task, NULL, 10, 1024 * 16, NULL);

    nwy_hal_uart_printf(g_dbg_uart_fd, "[READY v%s] Alert: %s\r\n",
                        CodeVer, data_collector_get_alert_phone(), remote_sd_get_server_url());

    /* Main heartbeat loop */
    uint32_t hb_counter = 0;
    while (1) {
        nwy_hal_os_thread_sleep(10000); /* 10s heartbeat */
        hb_counter++;

        /* Active runtime sensor connection verification */
        bool bme_online = bme68x_check_connection(&g_bme_dev);

        /* Auto-hotplug detection: If previously offline and now detected, auto-initialize */
        if (bme_online && g_bme_dev.chip_id != BME68X_CHIP_ID) {
            nwy_hal_uart_printf(g_dbg_uart_fd, "[HOTPLUG] BME688 sensor plugged in! Auto-initializing...\r\n");
            data_collector_check_sensor_health();
        }

        /* Check cellular IP status */
        char cur_ip[32] = "N/A";
        bool ip_connected = nwy_hal_net_get_ip(1, 1, cur_ip, sizeof(cur_ip));
        if (!ip_connected && nwy_hal_sim_is_ready(1) && (hb_counter % 6 == 0)) {
            /* Retry data call every minute if disconnected */
            nwy_hal_net_start_data_call(1, 1, NULL);
        }

        collector_status_t cur_status;
        data_collector_get_status(&cur_status, NULL, 0);

        nwy_hal_uart_printf(g_dbg_uart_fd, "[HEARTBEAT #%lu] State: %s | Sensor: %s | SD: %s | IP: %s\r\n",
                            (unsigned long)hb_counter,
                            (cur_status.state == COLLECTOR_STATE_RECORDING) ? "RECORDING" : "IDLE",
                            bme_online ? "OK (0x76)" : "DISCONNECTED",
                            nwy_sdcard_is_mounted() ? "OK" : "DISCONNECTED",
                            ip_connected ? cur_ip : "DISCONNECTED");
    }
}

/* -------------------------------------------------------------------------
 * OPENTOOLS APPLICATION ENTRY POINT
 * ------------------------------------------------------------------------- */
#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry(void)
{
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "app_main", 50, app_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#else
int appimg_enter(void *param)
{
    (void)param;
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "app_main", 50, app_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#endif

void appimg_exit(void)
{
    if (g_dbg_uart_fd >= 0) {
        nwy_uart_close(g_dbg_uart_fd);
        g_dbg_uart_fd = -1;
    }
}
