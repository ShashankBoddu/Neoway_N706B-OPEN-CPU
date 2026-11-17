/**
 * @file main.c
 * @brief Neoway N706B OpenCPU — Real-Time IP Solution (Isopropanol) TinyML Detector
 * 
 * Target Platform: Neoway N706B-CN / N706B-CN-10 (ASR1605S Crane ARM Cortex-R5 @ 614 MHz)
 * Sensor: SmartElex Environmental Sensor BME688 on I2C1 (Pin 57 SCL, Pin 58 SDA)
 * Model: Bosch Sensortec BME AI-Studio v3.2.1 Trained Neural Network
 * Alerting: 3GPP Cellular SMS Alert to +918074638788 (Strictly ONE SMS upon confirmation)
 */

#include "bme68x.h"
#include "i2c_bus.h"
#include "ip_detector.h"
#include "tinyml_nn.h"
#include "hal/sms/nwy_hal_sms.h"
#include "hal/sim/nwy_hal_sim.h"
#include "hal/uart/nwy_hal_uart.h"
#include "hal/os/nwy_hal_os.h"
#include "hal/pm/nwy_hal_pm.h"
#include "nwy_gpio_api.h"
#include "nwy_pm_api.h"
#include <stdio.h>
#include <string.h>

#define DEBUG_UART_PORT          "URT1"
#define DEBUG_UART_BAUD          115200
#define PIN_PERIPHERAL_PWR_GPIO0 0       /* Module Pin 83 (GPIO_00: Peripheral Master Gate) */
#define BME688_I2C_ADDR          0x76    /* Default SmartElex BME688 I2C address */

static int                 g_dbg_uart_fd = -1;
static bme68x_dev_t        g_bme_dev;
static uint8_t             g_bme_addr = BME688_I2C_ADDR;
static nwy_osi_semaphore_t g_sms_sem = NULL;
static volatile int        g_pending_sms_index = -1;
static nwy_osi_thread_t    g_sms_worker_thread = NULL;

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
    g_pending_sms_index = sms_index;
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
    nwy_hal_uart_printf(g_dbg_uart_fd, "[SMS WORKER] SMS listener thread active on SIM 1.\r\n");

    while (1) {
        nwy_hal_os_semaphore_acquire(g_sms_sem, 0xFFFFFFFF);

        if (g_pending_sms_index >= 0) {
            int idx = g_pending_sms_index;
            g_pending_sms_index = -1;

            nwy_sms_recv_info_type_t sms_data;
            memset(&sms_data, 0, sizeof(sms_data));

            if (nwy_hal_sms_read(1, idx, &sms_data)) {
                nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n======================================================\r\n");
                nwy_hal_uart_printf(g_dbg_uart_fd, ">>> INCOMING SMS COMMAND RECEIVED <<<\r\n");
                nwy_hal_uart_printf(g_dbg_uart_fd, "  * From Phone : %s\r\n", sms_data.source_phone_num);
                nwy_hal_uart_printf(g_dbg_uart_fd, "  * Command    : \"%s\"\r\n", sms_data.msg_content);
                nwy_hal_uart_printf(g_dbg_uart_fd, "======================================================\r\n");

                ip_detector_handle_sms_command(sms_data.source_phone_num, (const char *)sms_data.msg_content);
                nwy_hal_sms_delete(1, idx);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * SYSTEM BOOT BANNER
 * ------------------------------------------------------------------------- */
static void print_system_banner(void)
{
    int boot_reason = nwy_pm_boot_res();
    const char *reason_str = "Normal/Other";
    if (boot_reason == 1) reason_str = "Power Key";
    else if (boot_reason == 2) reason_str = "Pin Reset";
    else if (boot_reason == 3) reason_str = "Alarm";
    else if (boot_reason == 5) reason_str = "Watchdog";
    else if (boot_reason == 10) reason_str = "Panic Reset";

    nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, "==============================================================\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, "*    NEOWAY N706B — TINYML IP SOLUTION (ISOPROPANOL) DETECTOR *\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, "==============================================================\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Target Chemical: Isopropanol (IP Solution) vs Normal Air\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Model Quality  : 100.00%% Accuracy | 100.00%% F1 Score | 0.00%% FP\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Training Base  : Bosch Sensortec BME AI-Studio v3.2.1\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Neural Net Arch: Dense(10, ReLU) -> Dense(10, ReLU) -> Softmax(2)\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Sensor Target  : SmartElex BME688 MEMS AI Gas Scanner (HP-354)\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Alert Policy   : Confirmed over 3 cycles (~36s) -> STRICTLY 1 SMS\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Alert Number   : %s\r\n", DEFAULT_ALERT_PHONE);
    nwy_hal_uart_printf(g_dbg_uart_fd, " Remote SMS Cmds: Send 'STATUS' for live reading, 'RESET' to re-arm\r\n");
    nwy_hal_uart_printf(g_dbg_uart_fd, " Boot Cause     : %d (%s) | Built: %s %s\r\n", boot_reason, reason_str, __DATE__, __TIME__);
    nwy_hal_uart_printf(g_dbg_uart_fd, "==============================================================\r\n\r\n");
}

/* -------------------------------------------------------------------------
 * MAIN APPLICATION ENTRY POINT
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
    nwy_hal_uart_printf(g_dbg_uart_fd, "[1/5] Powering peripheral rails (GPIO 00 = HIGH)...\r\n");
    nwy_gpio_direction_set(PIN_PERIPHERAL_PWR_GPIO0, PIN_DIRECTION_OUT);
    nwy_gpio_value_set(PIN_PERIPHERAL_PWR_GPIO0, PIN_LEVEL_HIGH);
    nwy_hal_os_thread_sleep(100);

    /* 3. Initialize I2C Bus 1 Subsystem */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[2/5] Initializing I2C Bus 1 (Pin 57 SCL, Pin 58 SDA)...\r\n");
    i2c_bus_config_t i2c_cfg;
    i2c_cfg.bus_name   = I2C1_BUS_NAME;
    i2c_cfg.speed_hz   = I2C_DEFAULT_SPEED_HZ;
    i2c_cfg.scl_gpio   = I2C1_DEFAULT_SCL_GPIO;
    i2c_cfg.sda_gpio   = I2C1_DEFAULT_SDA_GPIO;
    i2c_cfg.mode       = I2C_MODE_AUTO;
    i2c_cfg.timeout_ms = I2C_DEFAULT_TIMEOUT_MS;
    i2c_bus_init(&i2c_cfg);

    /* 4. Detect and Initialize Bosch BME688 Sensor */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[3/5] Probing SmartElex BME688 at address 0x%02X...\r\n", g_bme_addr);
    bool ack = i2c_bus_ping(g_bme_addr);
    if (!ack && i2c_bus_ping(0x77)) {
        g_bme_addr = 0x77;
        ack = true;
    }

    if (ack) {
        memset(&g_bme_dev, 0, sizeof(g_bme_dev));
        g_bme_dev.dev_addr = g_bme_addr;
        g_bme_dev.intf_ptr = &g_bme_addr;
        g_bme_dev.read     = bme_i2c_read;
        g_bme_dev.write    = bme_i2c_write;
        g_bme_dev.delay_us = bme_delay_us;
        g_bme_dev.amb_temp = 25.0f;

        int8_t rslt = bme68x_init(&g_bme_dev);
        if (rslt == BME68X_OK && g_bme_dev.chip_id == BME68X_CHIP_ID) {
            nwy_hal_uart_printf(g_dbg_uart_fd, "  [BME688 READY] Bosch BME688 initialized! Chip ID: 0x%02X\r\n", g_bme_dev.chip_id);
        } else {
            nwy_hal_uart_printf(g_dbg_uart_fd, "  ERROR: bme68x_init failed (err: %d, id: 0x%02X)\r\n", rslt, g_bme_dev.chip_id);
        }
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  ERROR: No I2C ACK from BME688! Check wiring on Pin 57 / 58.\r\n");
    }

    /* 5. Initialize SIM & Arm 3GPP SMS Engine */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[4/5] Arming 3GPP Cellular SMS Engine on SIM 1...\r\n");
    nwy_hal_os_semaphore_create(&g_sms_sem, 0, 1);

    int sim_wait = 0;
    while (!nwy_hal_sim_is_ready(1) && sim_wait < 10) {
        nwy_hal_os_thread_sleep(1000);
        sim_wait++;
    }

    if (nwy_hal_sim_is_ready(1)) {
        char phone_num[32] = {0};
        nwy_hal_sim_get_msisdn(1, phone_num, sizeof(phone_num));
        nwy_hal_uart_printf(g_dbg_uart_fd, "  SIM 1 is READY! Local MSISDN: %s\r\n",
                            phone_num[0] ? phone_num : "Unknown");

        nwy_hal_sms_init(1, NWY_SMS_STORAGE_TYPE_UIM);
        nwy_hal_sms_register_recv_cb(1, on_incoming_sms_cb);
        nwy_thread_create(&g_sms_worker_thread, "sms_worker", 45,
                          sms_worker_task, NULL, 10, 1024 * 8, NULL);
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "  WARNING: SIM 1 not ready. SMS notifications will retry when network attaches.\r\n");
    }

    /* 6. Initialize IP Detector with BME688 and start continuous inference */
    nwy_hal_uart_printf(g_dbg_uart_fd, "[5/5] Launching TinyML Gas Inference Engine...\r\n");
    if (ip_detector_init(&g_bme_dev)) {
        ip_detector_start();
        nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n>>> SYSTEM FULLY ARMED. MONITORING GAS SENSOR FOR IP SOLUTION <<<\r\n\r\n");
    } else {
        nwy_hal_uart_printf(g_dbg_uart_fd, "\r\n>>> SENSOR ERROR: Connect BME688 sensor to proceed <<<\r\n\r\n");
    }

    /* Main supervisor watchdog loop */
    uint32_t hb = 0;
    while (1) {
        nwy_hal_os_thread_sleep(15000);
        hb++;
        detector_status_t st;
        ip_detector_get_status(&st);
        nwy_hal_uart_printf(g_dbg_uart_fd, "[SUPERVISOR #%lu] State: %s | Cycles: %lu | IP Conf: %.1f%% | SMS Sent: %s\r\n",
                            (unsigned long)hb,
                            (st.state == DETECTOR_STATE_ALERT_LATCHED) ? "CONFIRMED_IP (LATCHED)" :
                            (st.state == DETECTOR_STATE_CONFIRMING) ? "CONFIRMING_IP" : "MONITORING_AIR",
                            (unsigned long)st.cycle_count,
                            st.last_ip_confidence * 100.0f,
                            st.sms_alert_sent ? "YES" : "NO");
    }
}

int nwy_open_app_entry(void)
{
    nwy_osi_thread_t main_thread = NULL;
    nwy_thread_create(&main_thread, "app_main", 40,
                      app_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}

int appimg_enter(void *param)
{
    (void)param;
    nwy_osi_thread_t main_thread = NULL;
    nwy_thread_create(&main_thread, "app_main", 40,
                      app_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}

void appimg_exit(void)
{
    ip_detector_stop();
}
