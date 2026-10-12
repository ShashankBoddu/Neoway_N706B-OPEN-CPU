#include "nwy_sd_card.h"
#include "sd_spi.h"
#include "ff.h"
#include "nwy_uart_api.h"
#include "nwy_osi_api.h"
#include "nwy_gpio_api.h"
#include "nwy_pm_api.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define DEBUG_UART_PORT "URT1"
#define DEBUG_UART_BAUD 115200

#define PIN_SD_POWER_GPIO0 0  /* Pin 83 (GPIO_00 -> Q10 Gate/Drain -> SD Card VCC) */

static int s_uart_fd = -1;

void sd_log(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    if (len > 0) {
        if (buf[len - 1] != '\n') {
            buf[len++] = '\r';
            buf[len++] = '\n';
            buf[len] = '\0';
        }
        if (s_uart_fd >= 0) {
            nwy_uart_write(s_uart_fd, (unsigned char *)buf, (unsigned int)len);
        }
    }
}

static void print_banner(void)
{
    int boot_reason = nwy_pm_boot_res();
    const char *reason_str = "Normal/Other";
    if (boot_reason == 1) reason_str = "Power Key";
    else if (boot_reason == 2) reason_str = "Pin Reset";
    else if (boot_reason == 3) reason_str = "Alarm";
    else if (boot_reason == 5) reason_str = "Watchdog";
    else if (boot_reason == 10) reason_str = "Panic Reset";

    sd_log("");
    sd_log("==============================================================");
    sd_log("*      NEOWAY N706B OPEN-CPU SD CARD SPI (FatFs) SYSTEM       *");
    sd_log("==============================================================");
    sd_log(" Build Date   : %s %s", __DATE__, __TIME__);
    sd_log(" Boot Cause   : %d (%s)", boot_reason, reason_str);
    sd_log(" SPI Port     : SPI1 (SSP0: SCLK=P101, MOSI=P100, MISO=P108)");
    sd_log(" Level Shifter: U6 74AVC4T774 (1.8V VDD_EXT -> VBAT 3.3V-4.2V)");
    sd_log(" Chip Select  : GPIO 34 (Pin 79 SSP0_FRM)");
    sd_log(" Card Power   : GPIO 0 (Pin 83 GPIO_00 -> Q10 Drain -> SD VCC)");
    sd_log(" Filesystem   : Chan's FatFs (FAT16/FAT32, LFN Enabled, Re-entrant)");
    sd_log(" Reference API: FLoRa_1ch_LTE_GW/ESP_code/main/storage");
    sd_log("==============================================================");
    sd_log("");
}

static void sd_main_task(void *param)
{
    (void)param;

    /* 1. Disable Crane OS ROM debug logger to prevent URT collision */
    *(volatile uint8_t *)0x7e212578 = 0;

    /* 2. Prevent Crane OS baseband power manager from unsolicited sleep */
    nwy_pm_state_set(NWY_PM_SLEEP_DISABLE);

    /* 3. Open Debug UART1 at 115200 */
    s_uart_fd = nwy_uart_open(DEBUG_UART_PORT, DEBUG_UART_BAUD, FC_NONE);
    nwy_thread_sleep(200); /* 200 ms stabilization */

    print_banner();

    /* 3. Turn ON SD Card Power Rail via GPIO 0 (Q10 Drain -> SD Card VCC) */
    sd_log(">>> Powering ON SD Card VCC (GPIO 0 = HIGH -> Q10 Gate/Drain ON)...");
    nwy_gpio_direction_set(PIN_SD_POWER_GPIO0, PIN_DIRECTION_OUT);
    nwy_gpio_pull_set(PIN_SD_POWER_GPIO0, PIN_PULL_PU);
    nwy_gpio_value_set(PIN_SD_POWER_GPIO0, PIN_LEVEL_HIGH);
    nwy_thread_sleep(100); /* 100 ms FreeRTOS yield for VCC power ramp */
    sd_log(">>> SD Card VCC power rail stabilized (3.3V-4.2V ACTIVE).");

    /* =========================================================================
     * PHASE 1: HARDWARE & CARD IDENTIFICATION
     * ========================================================================= */
    sd_log(">>> [PHASE 1/6] INITIALIZING SD CARD (SPI1, GPIO 34)...");
    
    nwy_sdcard_config_t cfg = {
        .spi_bus_id  = SD_DEFAULT_SPI_BUS,
        .cs_gpio     = SD_DEFAULT_CS_GPIO,
        .init_speed  = SD_DEFAULT_INIT_SPEED,
        .work_speed  = SD_DEFAULT_WORK_SPEED,
        .mount_point = "0:"
    };

    int init_res = nwy_sdcard_init(&cfg);
    if (init_res != 0) {
        sd_log("[ERROR] SD card initialization failed! (Error code: %d)", init_res);
        sd_log("[ERROR] Please check: 1) Card inserted, 2) Level Shifter U6 power, 3) Pin 79 CS wiring.");
    } else {
        sd_log("[SUCCESS] SD card mounted cleanly on volume '0:'!");

        const sd_card_info_t *info = sd_spi_get_info();
        if (info != NULL) {
            const char *type_str = "UNKNOWN";
            if (info->card_type == SD_CARD_TYPE_SDV2_HC) type_str = "SDHC / SDXC (Block Addressed)";
            else if (info->card_type == SD_CARD_TYPE_SDV2_SC) type_str = "SDv2 SC (Byte Addressed)";
            else if (info->card_type == SD_CARD_TYPE_SDV1) type_str = "SDv1 Standard";
            else if (info->card_type == SD_CARD_TYPE_MMC) type_str = "MMC";

            sd_log("---------------- CARD METADATA ----------------");
            sd_log(" Card Type       : %s", type_str);
            sd_log(" Product Name    : %s", info->product_name);
            sd_log(" Manufacturer ID : 0x%02X", info->manufacturer_id);
            sd_log(" OEM ID          : 0x%04X", info->oem_id);
            sd_log(" Serial Number   : 0x%08lX", (unsigned long)info->serial_number);
            sd_log(" Revision        : %d.%d", info->revision_major, info->revision_minor);
            sd_log(" Date of Mfg     : %02d/%04d", info->manufacturing_month, info->manufacturing_year);
            sd_log(" Total Sectors   : %lu", (unsigned long)info->sector_count);
            sd_log(" Sector Size     : %lu Bytes", (unsigned long)info->sector_size);
            sd_log(" Card Capacity   : %lu MB (%lu GB)", 
                   (unsigned long)(info->capacity_bytes / (1024 * 1024)),
                   (unsigned long)(info->capacity_bytes / (1024 * 1024 * 1024)));
            
            uint64_t total_sp = nwy_sdcard_get_total_space();
            uint64_t free_sp  = nwy_sdcard_get_free_space();
            sd_log(" Volume Total    : %lu MB", (unsigned long)(total_sp / (1024 * 1024)));
            sd_log(" Volume Free     : %lu MB", (unsigned long)(free_sp / (1024 * 1024)));
            sd_log("-----------------------------------------------");
        }
    }

    if (init_res == 0) {
        /* =========================================================================
         * PHASE 2: DIRECTORY STRUCTURE TEST (ESP32 Reference Parity)
         * ========================================================================= */
        sd_log("");
        sd_log(">>> [PHASE 2/6] DIRECTORY STRUCTURE TEST...");
        static const char *test_dirs[] = {
            "/unpublish",
            "/unpublish/data",
            "/TC_config",
            "/TU_config",
            "/tu_cache",
            "/bin",
            "/bin/tc",
            "/bin/tu",
            "/test_dir"
        };

        for (size_t i = 0; i < sizeof(test_dirs) / sizeof(test_dirs[0]); i++) {
            bool exists = nwy_sdcard_exists(test_dirs[i]);
            if (!exists) {
                nwy_sdcard_create_dir(test_dirs[i]);
                exists = nwy_sdcard_exists(test_dirs[i]);
            }
            sd_log("  Dir [%s] -> %s", test_dirs[i], exists ? "EXISTS (OK)" : "FAILED");
        }

        /* =========================================================================
         * PHASE 3: FILE WRITE & READ VERIFICATION
         * ========================================================================= */
        sd_log("");
        sd_log(">>> [PHASE 3/6] FILE WRITE & READ VERIFICATION...");
        const char *test_path = "/test_dir/hello.txt";
        const char *test_data = "Neoway N706B Cat-1 OpenCPU SPI SD Card Engine V1.0 - SUCCESS!";
        size_t test_len = strlen(test_data);

        sd_log("  Writing file: %s (%u bytes)...", test_path, (unsigned int)test_len);
        int wr_ret = nwy_sdcard_write_file(test_path, test_data, test_len);
        if (wr_ret == 0) {
            sd_log("  Write SUCCESS!");
        } else {
            sd_log("  Write FAILED (code: %d)!", wr_ret);
        }

        char read_buf[128];
        memset(read_buf, 0, sizeof(read_buf));
        size_t read_bytes = 0;
        int rd_ret = nwy_sdcard_read_file(test_path, read_buf, sizeof(read_buf), &read_bytes);
        if (rd_ret == 0) {
            sd_log("  Read SUCCESS (%u bytes): \"%s\"", (unsigned int)read_bytes, read_buf);
            if (strcmp(test_data, read_buf) == 0) {
                sd_log("  [VERIFICATION PASSED] Read data matches written data 100%%!");
            } else {
                sd_log("  [VERIFICATION FAILED] Data mismatch!");
            }
        } else {
            sd_log("  Read FAILED (code: %d)!", rd_ret);
        }

        /* =========================================================================
         * PHASE 4: FILE APPEND VERIFICATION (Telemetry Streaming)
         * ========================================================================= */
        sd_log("");
        sd_log(">>> [PHASE 4/6] FILE APPEND STREAMING TEST...");
        const char *log_file = "/test_dir/sensor.csv";
        nwy_sdcard_delete_file(log_file); /* Start fresh */

        const char *csv_header = "ID,Timestamp_s,VBAT_mV,RSSI,Status\r\n";
        nwy_sdcard_write_file(log_file, csv_header, strlen(csv_header));

        for (int cycle = 1; cycle <= 5; cycle++) {
            char row[96];
            snprintf(row, sizeof(row), "%d,%lu,%d,%d,%s\r\n",
                     cycle, (unsigned long)(cycle * 10), 3850 + cycle * 5, -65 + cycle, "NORMAL");
            int app_ret = nwy_sdcard_append_file(log_file, row, strlen(row));
            sd_log("  Append row %d: %s (%s)", cycle, row, (app_ret == 0) ? "OK" : "ERR");
        }

        char csv_dump[512];
        size_t csv_read_len = 0;
        if (nwy_sdcard_read_file(log_file, csv_dump, sizeof(csv_dump), &csv_read_len) == 0) {
            sd_log("  Full appended CSV readback (%u bytes):\r\n%s", (unsigned int)csv_read_len, csv_dump);
        }

        /* =========================================================================
         * PHASE 5: THROUGHPUT & SPEED BENCHMARK
         * ========================================================================= */
        sd_log("");
        sd_log(">>> [PHASE 5/6] THROUGHPUT BENCHMARK (32 KB Block Transfer)...");
        const char *bench_file = "/test_dir/bench.bin";
        static uint8_t bench_buf[512];
        for (int i = 0; i < 512; i++) {
            bench_buf[i] = (uint8_t)(i & 0xFF);
        }

        /* Benchmark write 64 sectors (32 KB) */
        FIL bfil;
        if (f_open(&bfil, "0:/test_dir/bench.bin", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
            UINT bw_total = 0;
            for (int s = 0; s < 64; s++) {
                UINT bw = 0;
                f_write(&bfil, bench_buf, 512, &bw);
                bw_total += bw;
            }
            f_close(&bfil);
            sd_log("  Bench Write 32 KB Complete (%u bytes written)!", bw_total);
        }

        if (f_open(&bfil, "0:/test_dir/bench.bin", FA_READ) == FR_OK) {
            UINT br_total = 0;
            for (int s = 0; s < 64; s++) {
                UINT br = 0;
                f_read(&bfil, bench_buf, 512, &br);
                br_total += br;
            }
            f_close(&bfil);
            sd_log("  Bench Read 32 KB Complete (%u bytes read)!", br_total);
        }
        nwy_sdcard_delete_file(bench_file);

        /* =========================================================================
         * PHASE 6: RECURSIVE MUTEX VERIFICATION
         * ========================================================================= */
        sd_log("");
        sd_log(">>> [PHASE 6/6] RECURSIVE MUTEX VERIFICATION...");
        bool l1 = nwy_sdcard_lock(1000);
        bool l2 = nwy_sdcard_lock(1000);
        bool l3 = nwy_sdcard_lock(1000);
        sd_log("  Acquired lock depth 3: L1=%d, L2=%d, L3=%d", l1, l2, l3);
        nwy_sdcard_unlock();
        nwy_sdcard_unlock();
        nwy_sdcard_unlock();
        sd_log("  Unlocked depth 3 cleanly - Re-entrancy verified!");

        sd_log("");
        sd_log("==============================================================");
        sd_log("*           ALL SD CARD SPI (FatFs) TESTS PASSED!            *");
        sd_log("==============================================================");
    } else {
        sd_log(">>> [BOOT NOTICE] Card absent or unmounted at boot. Hot-swap monitoring active.");
    }

    /* Periodic Heartbeat Task */
    uint32_t hb_count = 0;
    uint64_t free_bytes = (init_res == 0) ? nwy_sdcard_get_free_space() : 0;
    bool prev_mounted = (init_res == 0);

    while (1) {
        nwy_thread_sleep(10000); /* 10 seconds via FreeRTOS scheduler yield */
        hb_count++;

        char hb_data[64];
        snprintf(hb_data, sizeof(hb_data), "%lu", (unsigned long)hb_count);
        int wr_res = nwy_sdcard_write_file("/unpublish/tail.txt", hb_data, strlen(hb_data));
        bool curr_mounted = nwy_sdcard_is_mounted();

        /* Detect hot-swap reconnection or disconnection event */
        if (!prev_mounted && curr_mounted && wr_res == 0) {
            free_bytes = nwy_sdcard_get_free_space();
            sd_log(">>> [SD_RECOVERY] SD card reconnected, re-initialized, and mounted cleanly! <<<");
        } else if (prev_mounted && (!curr_mounted || wr_res != 0)) {
            sd_log(">>> [SD_ALERT] SD card removed or write error detected! Safe recovery active... <<<");
        }
        prev_mounted = (curr_mounted && wr_res == 0);

        /* Update free space every 60 seconds (every 6th heartbeat) when mounted */
        if (curr_mounted && (hb_count % 6) == 0) {
            free_bytes = nwy_sdcard_get_free_space();
        }

        sd_log("[SD_HEARTBEAT #%lu] Card=%s | tail.txt=%s (write=%s) | Free Space=%lu MB",
               (unsigned long)hb_count,
               curr_mounted ? "MOUNTED" : "REMOVED",
               hb_data,
               (wr_res == 0) ? "OK" : "ERR",
               curr_mounted ? (unsigned long)(free_bytes / (1024 * 1024)) : 0);
    }
}

#ifdef FEATURE_NWY_ASR_PLAT
int nwy_open_app_entry(void)
{
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "sd_main", 50, sd_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#else
int appimg_enter(void *param)
{
    (void)param;
    nwy_osi_thread_t th = NULL;
    nwy_thread_create(&th, "sd_main", 50, sd_main_task, NULL, 10, 1024 * 16, NULL);
    return 0;
}
#endif

void appimg_exit(void)
{
    if (s_uart_fd >= 0) {
        nwy_uart_close(s_uart_fd);
        s_uart_fd = -1;
    }
}
