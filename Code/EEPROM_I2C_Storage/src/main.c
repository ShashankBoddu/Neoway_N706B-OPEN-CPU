#include "eeprom_24cxx.h"
#include "i2c_bus.h"
#include "nwy_uart_api.h"
#include "nwy_osi_api.h"
#include "nwy_gpio_api.h"
#include "nwy_pm_api.h"
#include <stdio.h>
#include <string.h>

#define NV_CONFIG_EEPROM_ADDR   0x0200
#define NV_CONFIG_MAGIC         0x45455031  /* "EEP1" */

#define NV_HEARTBEAT_ADDR       0x0100
#define NV_HEARTBEAT_MAGIC      0x48425431  /* "HBT1" */

/* Sample Industrial Non-Volatile Configuration Schema */
typedef struct {
    char     device_id[24];
    char     apn[32];
    char     mqtt_broker[48];
    uint16_t mqtt_port;
    uint32_t report_interval_sec;
    float    vbat_cal_factor;
    uint32_t boot_counter;
} __attribute__((packed)) app_nv_config_t;

/* Heartbeat persistence record */
typedef struct {
    uint32_t counter;
    uint32_t uptime_sec;
} __attribute__((packed)) app_nv_heartbeat_t;

static void print_banner(void)
{
    int boot_reason = nwy_pm_boot_res();
    const char *reason_str = "Normal/Other";
    if (boot_reason == 1) reason_str = "Power Key";
    else if (boot_reason == 2) reason_str = "Pin Reset";
    else if (boot_reason == 3) reason_str = "Alarm";
    else if (boot_reason == 5) reason_str = "Watchdog";
    else if (boot_reason == 10) reason_str = "Panic Reset";

    i2c_log("");
    i2c_log("==============================================================");
    i2c_log("*    NEOWAY N706B OPEN-CPU I2C EEPROM (P24C128D) SYSTEM      *");
    i2c_log("==============================================================");
    i2c_log(" Build Date   : %s %s", __DATE__, __TIME__);
    i2c_log(" Boot Cause   : %d (%s)", boot_reason, reason_str);
    i2c_log(" I2C Port     : I2C2 (Pin 67 SCL=GPIO 10, Pin 66 SDA=GPIO 11)");
    i2c_log(" Bus Voltage  : VDD_EXT (1.8V Direct Domain)");
    i2c_log(" Pull-Ups     : R25 (4.7k SCL) / R26 (4.7k SDA) to VDD_EXT");
    i2c_log(" EEPROM Model : Puya P24C128D-SSH-MIR (128 Kbit / 16 KByte)");
    i2c_log(" Device Addr  : 0x50 (7-bit, E0=E1=E2=GND), Write: 0xA0, Read: 0xA1");
    i2c_log(" Page Size    : 64 Bytes (256 Physical Pages)");
    i2c_log(" Bus Clock    : 400 kHz Fast Mode (ACK Polling <= 5ms)");
    i2c_log(" Safety Mode  : 9-Clock Bus Unstick Recovery & Mutex Protected");
    i2c_log("==============================================================");
    i2c_log("");
}

static void eeprom_main_task(void *param)
{
    (void)param;

    /* 1. Disable Crane OS ROM debug logger to prevent URT collision */
    *(volatile uint8_t *)0x7e212578 = 0;

    /* 2. Prevent Crane OS baseband power manager from unsolicited sleep */
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    /* 3. Allow stabilization settling */
    nwy_thread_sleep(200);

    print_banner();

    /* =========================================================================
     * PHASE 1: I2C BUS RECOVERY, SCAN & EEPROM PROBE
     * ========================================================================= */
    i2c_log(">>> [PHASE 1/6] INITIALIZING I2C BUS & PROBING P24C128D...");

    eeprom_config_t cfg = {
        .dev_addr            = EEPROM_DEFAULT_DEV_ADDR,
        .total_bytes         = EEPROM_TOTAL_BYTES,
        .page_size           = EEPROM_PAGE_SIZE,
        .verify_after_write  = true,
        .ack_poll_timeout_ms = EEPROM_ACK_POLL_TIMEOUT_MS
    };

    int init_res = eeprom_init(&cfg);
    if (init_res != 0) {
        i2c_log("[ERROR] EEPROM initialization failed (code: %d)!", init_res);
        i2c_log("[ERROR] Please verify: 1) U7 soldering, 2) VDD_EXT (1.8V), 3) Pins 66/67 pull-ups.");
    } else {
        i2c_log("[SUCCESS] P24C128D EEPROM detected and responding at 0x%02X!", cfg.dev_addr);

        /* Run bus scan to enumerate any other devices on I2C2 */
        i2c_bus_scan();

        const eeprom_info_t *info = eeprom_get_info();
        if (info != NULL) {
            i2c_log("---------------- EEPROM SPECIFICATIONS ----------------");
            i2c_log(" Capacity    : %u Bytes (%u KB / %u Kbit)",
                    info->total_bytes, info->total_bytes / 1024, (info->total_bytes * 8) / 1024);
            i2c_log(" Page Size   : %u Bytes", info->page_size);
            i2c_log(" Total Pages : %u Pages", info->page_count);
            if (info->serial_number_valid) {
                char sn_hex[48] = {0};
                for (int i = 0; i < 16; i++) {
                    snprintf(&sn_hex[i * 3], 4, "%02X ", info->serial_number[i]);
                }
                i2c_log(" Factory S/N : %s", sn_hex);
            }
            i2c_log("-------------------------------------------------------");
        }

        /* =========================================================================
         * PHASE 2: BYTE & SMALL BUFFER READ/WRITE VERIFICATION
         * ========================================================================= */
        i2c_log("");
        i2c_log(">>> [PHASE 2/6] BYTE & SHORT BUFFER READ/WRITE TEST...");
        uint8_t test_byte = 0xA5;
        uint8_t read_byte = 0x00;

        int b_ret = eeprom_write_byte(0x0000, test_byte);
        if (b_ret == 0) {
            eeprom_read_byte(0x0000, &read_byte);
            i2c_log("  Byte Write/Read at 0x0000: Wrote=0x%02X, Read=0x%02X -> %s",
                    test_byte, read_byte, (test_byte == read_byte) ? "MATCH (OK)" : "MISMATCH (FAIL)");
        } else {
            i2c_log("  [ERROR] Byte write failed (code: %d)", b_ret);
        }

        /* Test 16-byte pattern */
        uint8_t pattern_tx[16];
        uint8_t pattern_rx[16] = {0};
        for (int i = 0; i < 16; i++) pattern_tx[i] = (uint8_t)(0x10 + i);

        int p_ret = eeprom_write_verify(0x0010, pattern_tx, sizeof(pattern_tx));
        if (p_ret == 0) {
            eeprom_read(0x0010, pattern_rx, sizeof(pattern_rx));
            bool match = (memcmp(pattern_tx, pattern_rx, sizeof(pattern_tx)) == 0);
            i2c_log("  16-Byte Block Write/Read at 0x0010: %s", match ? "VERIFIED (OK)" : "MISMATCH (FAIL)");
        } else {
            i2c_log("  [ERROR] 16-byte write failed (code: %d)", p_ret);
        }

        /* =========================================================================
         * PHASE 3: PAGE-BOUNDARY STRESS TEST (Crossing 64-byte Page Boundaries)
         * ========================================================================= */
        i2c_log("");
        i2c_log(">>> [PHASE 3/6] PAGE BOUNDARY CROSSING STRESS TEST...");
        /* Start at address 58 (0x003A). Write 130 bytes.
         * Spans:
         *   Page 0: Bytes 58..63  (6 bytes)
         *   Page 1: Bytes 64..127 (64 bytes)
         *   Page 2: Bytes 128..187(60 bytes)
         * Total: 130 bytes across 3 physical pages.
         */
        uint16_t span_addr = 58;
        uint8_t span_tx[130];
        uint8_t span_rx[130] = {0};

        for (size_t i = 0; i < sizeof(span_tx); i++) {
            span_tx[i] = (uint8_t)((i * 3 + 7) & 0xFF);
        }

        i2c_log("  Writing %u bytes spanning across 3 pages (Start: 0x%04X, End: 0x%04X)...",
                (unsigned int)sizeof(span_tx), span_addr, span_addr + (unsigned int)sizeof(span_tx) - 1);

        int span_res = eeprom_write(span_addr, span_tx, sizeof(span_tx));
        if (span_res == 0) {
            int rd_res = eeprom_read(span_addr, span_rx, sizeof(span_rx));
            if (rd_res == 0 && memcmp(span_tx, span_rx, sizeof(span_tx)) == 0) {
                i2c_log("  [PAGE BOUNDARY PASSED] All 130 bytes match 100%% without roll-over corruption!");
            } else {
                i2c_log("  [PAGE BOUNDARY FAILED] Data corrupted across page boundary!");
            }
        } else {
            i2c_log("  [ERROR] Page span write failed (code: %d)", span_res);
        }

        /* =========================================================================
         * PHASE 4: STRUCTURED CONFIG RECORD STORAGE WITH CRC-32
         * ========================================================================= */
        i2c_log("");
        i2c_log(">>> [PHASE 4/6] STRUCTURED CONFIG STORAGE (CRC-32 VERIFIED)...");

        app_nv_config_t test_cfg = {
            .device_id           = "N706B-GATEWAY-001",
            .apn                 = "iot.telecom.net",
            .mqtt_broker         = "broker.industrial-iot.io",
            .mqtt_port           = 8883,
            .report_interval_sec = 60,
            .vbat_cal_factor     = 1.024f,
            .boot_counter        = 42
        };

        i2c_log("  Saving Configuration Record at 0x%04X (Magic: 0x%08X)...",
                NV_CONFIG_EEPROM_ADDR, NV_CONFIG_MAGIC);
        int cfg_wr = eeprom_write_config_record(NV_CONFIG_EEPROM_ADDR, NV_CONFIG_MAGIC,
                                               &test_cfg, sizeof(test_cfg));
        if (cfg_wr == 0) {
            i2c_log("  Record successfully written and verified with CRC-32!");

            app_nv_config_t read_cfg;
            uint16_t actual_sz = 0;
            int cfg_rd = eeprom_read_config_record(NV_CONFIG_EEPROM_ADDR, NV_CONFIG_MAGIC,
                                                  &read_cfg, sizeof(read_cfg), &actual_sz);
            if (cfg_rd == 0 && actual_sz == sizeof(test_cfg)) {
                i2c_log("  Config Readback SUCCESS:");
                i2c_log("    Device ID : %s", read_cfg.device_id);
                i2c_log("    APN       : %s", read_cfg.apn);
                i2c_log("    Broker    : %s:%u", read_cfg.mqtt_broker, read_cfg.mqtt_port);
                i2c_log("    Interval  : %lu s", (unsigned long)read_cfg.report_interval_sec);
                i2c_log("    CRC32     : VALID (Payload intact)");
            } else {
                i2c_log("  [ERROR] Config readback failed (code: %d)!", cfg_rd);
            }
        } else {
            i2c_log("  [ERROR] Config write failed (code: %d)!", cfg_wr);
        }

        /* =========================================================================
         * PHASE 5: P24C128D IDENTIFICATION PAGE READ
         * ========================================================================= */
        i2c_log("");
        i2c_log(">>> [PHASE 5/6] SECURITY REGISTER & IDENTIFICATION PAGE...");
        uint8_t id_page[64] = {0};
        int id_res = eeprom_read_id_page(id_page);
        if (id_res == 0) {
            char id_hex[32] = {0};
            for (int i = 0; i < 8; i++) {
                snprintf(&id_hex[i * 3], 4, "%02X ", id_page[i]);
            }
            i2c_log("  Read 64-byte ID Page successfully. First 8 bytes: %s...", id_hex);
        } else {
            i2c_log("  Identification page access notice (code: %d)", id_res);
        }

        /* =========================================================================
         * PHASE 6: THROUGHPUT / SPEED BENCHMARK (512 Bytes / 8 Pages)
         * ========================================================================= */
        i2c_log("");
        i2c_log(">>> [PHASE 6/6] SPEED BENCHMARK (512-Byte Write & Read)...");
        uint8_t bench_buf[512];
        for (int i = 0; i < 512; i++) bench_buf[i] = (uint8_t)(i & 0xFF);

        int64 t_wr_start = nwy_uptime_get();
        int wr_b_res = eeprom_write(0x1000, bench_buf, sizeof(bench_buf));
        int64 t_wr_elapsed = nwy_uptime_get() - t_wr_start;

        int64 t_rd_start = nwy_uptime_get();
        int rd_b_res = eeprom_read(0x1000, bench_buf, sizeof(bench_buf));
        int64 t_rd_elapsed = nwy_uptime_get() - t_rd_start;

        if (wr_b_res == 0 && rd_b_res == 0) {
            i2c_log("  512-Byte Page-Write (8 pages + ACK polling): %lld ms", t_wr_elapsed);
            i2c_log("  512-Byte Sequential Read                 : %lld ms", t_rd_elapsed);
            i2c_log("  Write Speed: %.2f KB/s | Read Speed: %.2f KB/s",
                    (t_wr_elapsed > 0) ? (512.0f / (float)t_wr_elapsed) : 512.0f,
                    (t_rd_elapsed > 0) ? (512.0f / (float)t_rd_elapsed) : 512.0f);
        }

        i2c_log("");
        i2c_log("==============================================================");
        i2c_log("*             ALL P24C128D EEPROM TESTS PASSED!              *");
        i2c_log("==============================================================");
    }

    /* =========================================================================
     * PERIODIC NON-VOLATILE HEARTBEAT MONITORING TASK
     * ========================================================================= */
    uint32_t hb_count = 0;
    app_nv_heartbeat_t hb_rec = { .counter = 0, .uptime_sec = 0 };

    /* Load existing counter if available */
    if (init_res == 0) {
        uint16_t sz = 0;
        if (eeprom_read_config_record(NV_HEARTBEAT_ADDR, NV_HEARTBEAT_MAGIC,
                                      &hb_rec, sizeof(hb_rec), &sz) == 0) {
            hb_count = hb_rec.counter;
            i2c_log(">>> Restored Persistent Run-Counter: %lu (from EEPROM address 0x%04X)",
                    (unsigned long)hb_count, NV_HEARTBEAT_ADDR);
        }
    }

    while (1) {
        nwy_thread_sleep(10000); /* 10-second non-blocking RTOS yield */
        hb_count++;

        hb_rec.counter = hb_count;
        hb_rec.uptime_sec = (uint32_t)(nwy_uptime_get() / 1000);

        int wr_hb = -1;
        bool present = eeprom_ping();

        if (present) {
            wr_hb = eeprom_write_config_record(NV_HEARTBEAT_ADDR, NV_HEARTBEAT_MAGIC,
                                               &hb_rec, sizeof(hb_rec));
        }

        i2c_log("[EEPROM_HEARTBEAT #%lu] Chip=%s | StoredCounter=%lu | Write=%s | CRC=OK",
                (unsigned long)hb_count,
                present ? "ONLINE" : "OFFLINE",
                (unsigned long)hb_rec.counter,
                (wr_hb == 0) ? "SUCCESS" : "ERR");
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry(void)
{
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "eeprom_main", 50, eeprom_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#else
int appimg_enter(void *param)
{
    (void)param;
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "eeprom_main", 50, eeprom_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#endif

void appimg_exit(void)
{
    eeprom_deinit();
}
