# Neoway N706B SD Card SPI (FatFs) Storage Engine

Industrial-grade SD Card (SPI Mode) storage driver and filesystem engine for the **Neoway N706B Cat-1 OpenCPU** module (ASR1605 Crane architecture), with full architectural parity to the **ESP32 LTE Gateway** storage layer (`FLoRa_1ch_LTE_GW/ESP_code/main/storage/sd_card.h`).

---

## 1. Hardware Pinout & Level Shifter Wiring

The N706B OpenCPU IO domain runs at **1.8V (`VDD_EXT`)**, while standard microSD cards operate at **3.3V / VBAT**. Level shifting is handled by **U6 (`74AVC4T774GUX`)**:

| N706B Module Pin | Net / Function | U6 Channel (74AVC4T774) | Level Shifted Net | MicroSD Socket Pin |
|---|---|---|---|---|
| **Pin 83** | `GPIO_00` / **GPIO 0** (Power Switch) | Q10 Gate/Drain Switch | `SD_VCC` Rail | Pin 4 (VDD / 3.3V) |
| **Pin 79** | `SSP0_FRM` / **GPIO 34** (Output) | A1 -> B1 (DIR1 = 1.8V HIGH) | `VBATSPI_CS` | Pin 1 (CS / DAT3) |
| **Pin 101** | `SSP0_SCLK` (Clock Output) | A2 -> B2 (DIR2 = 1.8V HIGH) | `VBATSPI_CLK` | Pin 5 (SCLK) |
| **Pin 100** | `SSP0_TXD` (MOSI Output) | A3 -> B3 (DIR3 = 1.8V HIGH) | `VBATSPI_MOSI` | Pin 2 (MOSI / CMD) |
| **Pin 108** | `SSP0_RXD` (MISO Input) | A4 <- B4 (DIR4 = GND LOW) | `VBATSPI_MISO` | Pin 7 (MISO / DAT0) |
| **Pin 14** | `VDD_EXT` (1.8V Reference) | VCC(A) | — | — |
| **Pin 13** | `VBAT` (Power Rail) | VCC(B) | — | Level Shifter Rail |
| **Pin 88-95**| System `GND` | OE# & GND (Pins 7, 8) | — | Pins 3, 6 (VSS / GND) |

> **Power Switch Control**: MicroSD VCC is gated via **Q10 MOSFET**, driven by **Pin 83 (GPIO 0)**. `main.c` and `sd_spi_hw_init()` assert `GPIO 0 = HIGH` to power up the SD card rail before starting SPI communication.
>
> **Note on Chip Select (CS)**: SD SPI specification requires CS to remain continuously asserted (LOW) across multi-byte command frames, variable-length response latency, and block data token transfers. Software GPIO control on **Pin 79 (GPIO 34)** is used to guarantee framing integrity.

---

## 2. Directory Structure

```
Code/SD_Card_Storage/
├── build_SD_Card_Storage.bat    # Automated Ninja build & release packager (.zip & .pkt)
├── README.md                    # Project documentation & reference
├── inc/
│   ├── nwy_sd_card.h            # High-level storage manager (ESP32 reference parity)
│   ├── sd_spi.h                 # Low-level SD SPI protocol & hardware driver header
│   ├── ff.h                     # Chan's FatFs core header (R0.12c)
│   ├── ffconf.h                 # FatFs configuration file (FAT16/FAT32, re-entrant)
│   ├── diskio.h                 # FatFs low-level disk I/O interface
│   └── integer.h                # FatFs integer type definitions
└── src/
    ├── main.c                   # Entry point & 6-phase test verification suite
    ├── nwy_sd_card.c            # Thread-safe storage manager with FreeRTOS recursive mutex
    ├── sd_spi.c                 # SD SPI protocol engine (CMD0..58, DMA aligned buffers)
    ├── ff.c                     # Chan's FatFs implementation
    └── diskio.c                 # FatFs diskio binding to sd_spi
```

---

## 3. Storage Architecture

### Low-Level Driver (`sd_spi.c` / `sd_spi.h`)
- **Initialization Frequency**: 400 kHz during identification (CMD0 -> CMD8 -> ACMD41 loop -> CMD58 OCR).
- **Operational Frequency**: Automatically switches up to **12 MHz** upon successful card identification.
- **Card Detection**: Automatically detects **SDHC / SDXC (Block Addressed)** vs **SDSC (Byte Addressed)** via OCR bit 30 (CCS).
- **Metadata Decoding**: Parses CID and CSD registers for Product Name, Serial Number, Manufacturer ID, OEM ID, Revision, Manufacturing Date, and total capacity.
- **Crane DMA Alignment**: All SPI transfer buffers are aligned to **16-byte boundaries (`__attribute__((aligned(16)))`)**, guaranteeing zero DMA cache alignment faults on the ASR1605 platform.

### Filesystem Layer (`ff.c` / `diskio.c`)
- Industry-standard **Chan's FatFs (R0.12c)** supporting FAT12, FAT16, and FAT32.
- Pre-configured 512-byte sectors (`_MIN_SS = 512`, `_MAX_SS = 512`).
- Safe file lock limits (`_FS_LOCK = 5`).
- Automatic formatting capability (`f_mkfs`) if an unformatted card is inserted.

### High-Level Manager (`nwy_sd_card.c` / `nwy_sd_card.h`)
Provides exact functional parity with the ESP32 `SDCard` class:
- `nwy_sdcard_init()` / `nwy_sdcard_deinit()`
- `nwy_sdcard_lock(timeout_ms)` / `nwy_sdcard_unlock()` — **FreeRTOS recursive mutex** ensuring safe concurrent access across threads.
- `nwy_sdcard_exists(path)`
- `nwy_sdcard_create_dir(path)` / `nwy_sdcard_remove_dir(path)` / `nwy_sdcard_remove_dir_recursive(path)`
- `nwy_sdcard_delete_file(path)`
- `nwy_sdcard_read_file(path, buffer, size, &out_len)`
- `nwy_sdcard_write_file(path, data, len)`
- `nwy_sdcard_append_file(path, data, len)`
- `nwy_sdcard_get_total_space()` / `nwy_sdcard_get_free_space()`
- `nwy_sdcard_format()`

---

## 4. Boot Verification Test Suite

When flashed, `main.c` boots up on UART1 (115200 baud) and executes a **6-Phase verification suite**:

1. **Phase 1: Card Identification**: Mounts card, reads CID/CSD, prints card type, product name, serial number, sector count, physical capacity, and volume free space.
2. **Phase 2: Directory Parity Test**: Creates and verifies all ESP32 reference directories:
   - `/unpublish`, `/unpublish/data`, `/TC_config`, `/TU_config`, `/tu_cache`, `/bin`, `/bin/tc`, `/bin/tu`, `/test_dir`
3. **Phase 3: Write & Read Verification**: Writes a test payload to `/test_dir/hello.txt`, reads it back, and verifies 100% byte-for-byte data integrity.
4. **Phase 4: Append Streaming Test**: Creates `/test_dir/sensor.csv` with a header and appends 5 telemetry rows, reading back the full stream.
5. **Phase 5: Throughput Benchmark**: Writes and reads a 32 KB block file (`/test_dir/bench.bin`), verifying sector block transfer reliability.
6. **Phase 6: Re-entrancy Test**: Performs nested recursive locking (depth 3) to verify that thread synchronization does not deadlock.
7. **Periodic Heartbeat**: Every 10 seconds, appends a counter to `/unpublish/tail.txt` and logs current volume free space.

---

## 5. Building & Flashing

### To Build:
Double-click `build_SD_Card_Storage.bat` or run:
```cmd
cd E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\SD_Card_Storage
build_SD_Card_Storage.bat
```

### Outputs Generated in `release/`:
- **`nwy_open_app.zip`**: Complete flash package for the **Aboot Download Tool**.
- **`nwy_open_app_fota.pkt`**: Differential / FOTA binary package.
