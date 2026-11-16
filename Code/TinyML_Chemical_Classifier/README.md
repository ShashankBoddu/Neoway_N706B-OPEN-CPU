# Smart TinyML Chemical & Odor Classifier (Neoway N706B OpenCPU)

An Edge AI gas classification and specimen data collection system designed for the **Neoway N706B OpenCPU** (ASR1605S Crane ARM Cortex-R5 @ 614 MHz) paired with the **SmartElex Environmental Sensor BME688** (Bosch MEMS 4-in-1 gas scanner) and a high-speed **MicroSD Card SPI** datalogger.

The system is controlled in real time via **3GPP SMS text messages** to start/stop labeled recording sessions (e.g., `TOILET_CLEAN`, `TOILET_DIRTY`, `IPA_SOLUTION`), alter scan rates, and query live sensor telemetry.

---

## 1. System Architecture

```
                       +---------------------------------------+
                       |        Neoway N706B OpenCPU           |
                       |    (ASR1605S ARM Cortex-R5 @ 614 MHz) |
                       +---------------------------------------+
                           |            |                |
                Pin 83     |     Pin 57 | Pin 58  Pin 79 | Pin 101, 100, 108
               (GPIO 00)   |    (I2C1)  | (I2C1)  (CS)   | (SPI1 / SSP0)
                   |       |            |                |
             [Load Switch] |     [Level Shifter]   [Level Shifter]
                Q3 & Q10   |       U8 (NXS0102)    U6 (74AVC4T774)
                   |       |            |                |
                   v       |            v                v
            VCC_SWout Rail |    SmartElex BME688   MicroSD Card Slot
            (3.3V - 4.2V)  |     (0x76 / 0x77)     (FatFs 0:/dataset_*.csv)
                           |
                           v
                     Cellular LTE / 2G Antenna
                     (SMS Command & Control)
```

---

## 2. Hardware Wiring & Pin Mapping

### A. SmartElex BME688 Breakout to Carrier Board Header

| BME688 Breakout Pin | Carrier Board Net | N706B Module Pin | Description |
|:---|:---|:---|:---|
| **VCC** | `VCC_SWout` | Gated by Pin 83 (GPIO 0) | Switched 3.3V–4.2V power rail (Q3/Q1 MOSFET) |
| **GND** | `GND` | Ground Pins | Common Ground |
| **SCL** | `VBATI2C1_SCL` | Pin 57 (`I2C1_SCL` / `GPIO[49]`) | Level-shifted to VBAT via U8 (NXS0102) |
| **SDA** | `VBATI2C1_SDA` | Pin 58 (`I2C1_SDA` / `GPIO[50]`) | Level-shifted to VBAT via U8 (NXS0102) |
| **SDO** | `GND` (or `VCC`) | — | Tie to GND for address `0x76`; tie to VCC for `0x77` |
| **CS** | `VCC` | — | Pull HIGH to enable I2C mode on the BME688 breakout |

### B. MicroSD Card SPI Interface

| SD Card Pin | Module Pin | Net Name | Hardware Path |
|:---|:---|:---|:---|
| **CS** | Pin 79 | `SSP0_FRM` | GPIO 34 through U6 (74AVC4T774) |
| **SCLK** | Pin 101 | `SSP0_SCLK` | Hardware SPI1 Clock (up to 12 MHz) |
| **MOSI** | Pin 100 | `SSP0_TXD` | Hardware SPI1 Transmit |
| **MISO** | Pin 108 | `SSP0_RXD` | Hardware SPI1 Receive |
| **VCC** | Pin 83 Gate | `VCC_SWout` | Switched through Q5/Q10 P-MOSFET |

### C. UART1 Debug Console

- **Baud Rate**: `115200 8-N-1`
- **Port**: `URT1` (`TXD` = Pin 28, `RXD` = Pin 27)

---

## 3. Two-Way SMS Command Protocol

The device uses an event-driven 3GPP SMS receiver. Messages received from any authorized phone are parsed, executed, deleted from SIM memory, and confirmed back via an outbound reply SMS.

### Command Dictionary

| SMS Command | Example | Description | Device SMS Reply Example |
|:---|:---|:---|:---|
| `START <LABEL>` | `START TOILET_CLEAN` | Starts a new specimen dataset file with the specified classification label. | `[BME688] RECORDING STARTED! File: 0:/dataset_TOILET_CLEAN_001.csv, Interval: 3s` |
| `STOP` | `STOP` | Stops the active recording session and syncs the CSV file safely to the SD card. | `[BME688] RECORDING STOPPED. Samples: 142. File: 0:/dataset_TOILET_CLEAN_001.csv` |
| `STATUS` | `STATUS` | Queries active state, sensor readings, and storage availability. | `[BME688 STATUS] State: RECORDING, Label: TOILET_CLEAN, Samples: 35, SD Free: 14820 MB, Temp: 26.4C, Gas: 184.2 kOhm` |
| `INTERVAL <SEC>` | `INTERVAL 5` | Configures the scanning interval in seconds (1 to 3600 s). | `[BME688] Scan interval updated to 5 seconds.` |
| `HELP` | `HELP` | Returns a list of supported SMS commands. | `Commands: START <LABEL>, STOP, STATUS, INTERVAL <SEC>, HELP` |

---

## 4. Bosch HP-354 Multi-Heater Scanning & Data Schema

The BME688 sensor executes a 10-step sequential heating profile (**HP-354**), cycling through 10 distinct temperatures (200 °C to 400 °C) with a 140 ms dwell time per step. Different VOC gases, odors, and vapors exhibit distinct resistance curves across these 10 temperatures.

### CSV Output Schema (`0:/dataset_<LABEL>_%03d.csv`)

```csv
timestamp_ms,sample_idx,label,temp_c,humidity_pct,pressure_hpa,gas_res_0_ohm,gas_res_1_ohm,gas_res_2_ohm,gas_res_3_ohm,gas_res_4_ohm,gas_res_5_ohm,gas_res_6_ohm,gas_res_7_ohm,gas_res_8_ohm,gas_res_9_ohm
14350,1,TOILET_CLEAN,25.82,48.15,1013.25,245120.0,268400.0,290150.0,312000.0,335000.0,350100.0,340200.0,320100.0,295000.0,260000.0
17410,2,TOILET_CLEAN,25.84,48.12,1013.24,245200.0,268550.0,290300.0,312150.0,335200.0,350250.0,340350.0,320250.0,295100.0,260150.0
```

- Each file corresponds to one specimen class run.
- Data is committed to FAT32 Flash storage on every sample cycle via `f_sync()`, preventing data corruption if power is removed unexpectedly.

---

## 5. Building & Flashing Firmware

### Step 1: Compile Firmware
Run the automated build script:
```cmd
cd E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\TinyML_Chemical_Classifier
build_TinyML_Chemical_Classifier.bat
```

### Step 2: Output Artifacts
The script creates:
1. `release/nwy_open_app.zip` — Complete release package for **Aboot USB Download Tool**.
2. `release/nwy_open_app_fota.pkt` — Signed FOTA delta/full upgrade package.
3. `../../N706B-A07-STD-OE_CN1X_ITRI-009_SDK/out/bin/nwy_open_app.bin` — Raw ARM ELF application binary.

### Step 3: Flash with Aboot Tool
1. Connect N706B USB to PC.
2. Open `tools\aboot\arelease.exe` (or Aboot GUI downloader).
3. Select `release\nwy_open_app.zip` and click **Download**.

---

## 6. Training the TinyML Model & Exporting to C

Once specimen datasets have been collected across various conditions (e.g., Clean Toilet, Dirty/Ammonia Odor, IPA Solution, Clean Air), extract the CSV files from the MicroSD card:

```cmd
python tools/train_classifier.py --data-dir path/to/csv/files --output-header inc/bme688_model.h --max-depth 6
```

### Generated Inference Header (`bme688_model.h`)
The script evaluates model accuracy, prints confusion matrices, and generates an optimized C decision tree function:

```c
bme688_class_e result = bme688_predict(temp_c, humidity_pct, pressure_hpa, gas_res_ohms);
if (result == CLASS_TOILET_DIRTY) {
    // Trigger automated ventilation, buzzer, or cellular cloud alert!
}
```
Inference latency on the Cortex-R5 is **under 10 microseconds** with negligible RAM footprint.
