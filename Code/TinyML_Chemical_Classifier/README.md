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

## 3. Two-Way SMS Command Protocol & Proactive Alerts

The device communicates via 3GPP cellular SMS. Incoming messages are parsed outside ISR context, executed immediately, and confirmed back to the sender handset.

### A. Proactive Alert System (Destination: `+918074638788`)

> [!NOTE]
> **Anti-Spam Latching:** Alerts are dispatched **strictly once per event** (not continuously). A state must recover before another alert can trigger.

1. **Boot Notification (Sent once on boot):**
   `[BME688 READY] Online! Sensor:OK(0x76), SD:OK(14.8GB), State:IDLE. Send 'START <LABEL>' (e.g. START BADFOOD)`
2. **Sensor Fault Alert (Sent once if I2C fails during a run):**
   `[BME688 ALERT] Sensor I2C read error (code -3) on 'BADFOOD'. Check wiring!`
3. **Sensor Recovery Notice (Sent once upon recovery):**
   `[BME688 INFO] Sensor recovered! Recording resumed for 'BADFOOD'.`
4. **Storage Space Alert (Sent once if SD free < 50MB):**
   `[BME688 ALERT] MicroSD nearly full (<50MB). Recording stopped!`

### B. Command Dictionary (Universal Label Collection, Remote SD, & FOTA)

You can collect data for **any environmental or odor condition** (e.g., `BADFOOD`, `TOILET_CLEAN`, `TOILET_DIRTY`, `IPA_SOLUTION`, `SPOILED_MILK`, `AMMONIA`), remotely list and download MicroSD files via HTTP, or perform secure FOTA updates:

| SMS Command | Example | Description | Device SMS Reply Example |
|:---|:---|:---|:---|
| `START <LABEL>` | `START BADFOOD` | Starts a new specimen dataset file with any custom label. | `STARTED: Dataset 'BADFOOD'. File: dataset_BADFOOD_001.csv. Sampling every 3s.` |
| `<LABEL>` *(Direct)*| `BADFOOD` | Typing the label directly also starts recording automatically! | `STARTED: Dataset 'BADFOOD'. File: dataset_BADFOOD_001.csv. Sampling every 3s.` |
| `STOP` | `STOP` | Stops the active recording session and syncs the CSV & BME files safely to the SD card. | `STOPPED: 'BADFOOD' (142 samples). Dual saved: dataset_BADFOOD_001.csv & .bmerawdata` |
| `STATUS` | `STATUS` | Queries active state, sensor readings, and storage availability. | `STATUS: RUNNING \| Label: BADFOOD \| Count: 35 \| T: 26.4C \| H: 48.1% \| R0: 184.2k \| SD: 14.8GB free` |
| `LIST` or `SD LIST` | `LIST` | Scans MicroSD card and returns file count and filenames via SMS / MQTT. | `SD Files (3):\n1. 20261117_1336_IP_0.bmerawdata (14K)\n2. 20261117_1336_IP_0.bmelabelinfo (1K)...` |
| `UPLOAD LATEST` | `UPLOAD LATEST` | Streams newest session files (`.bmerawdata` & `.bmelabelinfo`) to HTTP server. | `UPLOAD: Queued latest session files to server. Transferring in background...` |
| `UPLOAD ALL` | `UPLOAD ALL` | Iterates and uploads all `.bmerawdata`, `.bmelabelinfo`, and `.csv` files on SD card. | `UPLOAD: Queued ALL dataset files to server. Transferring in background...` |
| `UPLOAD <FILE>` | `UPLOAD IPA_0.bmerawdata` | Uploads a specific dataset file to the configured HTTP server. | `UPLOAD: Queued 'IPA_0.bmerawdata' to server. Transferring in background...` |
| `SERVER <URL>` | `SERVER https://<link>/upload` | Queries or sets the target HTTP server endpoint for remote SD uploads. | `SERVER: Upload URL set to https://<link>/upload` |
| `FOTA` | `FOTA` | Zero-configuration OTA: auto-derives URL, reads size/hash/ECDSA signature from HTTP headers, flashes, and reboots! | `FOTA: Download https://.../nwy_open_app_fota.pkt...` |
| `FOTA <URL> <SZ> <SIG>`| `FOTA https://... 83756 3044...` | Explicit OTA: downloads specific URL with verified size and signature. | `FOTA: Downloading 83756 bytes from https://... Verifying signature...` |
| `INTERVAL <SEC>` | `INTERVAL 5` | Configures the scanning interval in seconds (2 to 300 s). | `INTERVAL: Sampling rate set to 5 seconds.` |
| `ALERT <PHONE>` | `ALERT +918074638788` | Queries or updates the destination phone number for proactive alerts. | `Alerts recipient updated to: +918074638788` |
| `HELP` | `HELP` | Returns a list of supported SMS / MQTT commands. | `Commands: START <LBL>, STOP, STATUS, LIST, UPLOAD <TARGET>, SERVER <URL>, FOTA, INTERVAL, ALERT` |

---

## 4. Remote MicroSD Access & Cellular HTTP Receiver Tool

A dedicated multi-threaded Python server is included in `tools/http_receiver_server.py`. It provides two core functions:
1. **Cellular Dataset Receiver**: Receives dataset files uploaded by the Neoway N706B and stores them in `tools/received_datasets/`.
2. **Industrial FOTA Host**: Serves compiled firmware binaries (`nwy_open_app_fota.pkt`) with live ECDSA signing, RFC 7233 range clamping, 100ms flow pacing, and a 20s post-stream hold period.

### Launching the Server (with Zero-Drop Cloudflare Tunnel):
```cmd
python tools/http_receiver_server.py --port 8080
```
The server automatically launches a Cloudflare Quick Tunnel and outputs the public endpoint:
```
[+] Tunnel online (Cloudflare Quick Tunnel (HTTPS))! Public endpoint: https://<subdomain>.trycloudflare.com/upload
```

Configure the board via SMS, COM8 serial, or MQTT terminal:
```
SERVER https://<subdomain>.trycloudflare.com/upload
```

---

## 5. Secure Cellular FOTA (Firmware Over-The-Air) Updating

The firmware integrates the standalone **ECDSA secp256r1 + SHA-256** crypto engine (`src/crypto/`) utilizing lightweight Micro-ECC (`uECC`), replacing heavy mbedTLS and ensuring binary size stays within the 86 KB wear-leveled NOR flash partition.

### Zero-Touch FOTA Workflow:
1. Recompile and sign firmware:
   ```cmd
   build_TinyML_Chemical_Classifier.bat && python tools/fota_signer.py
   ```
2. Start the cellular receiver server:
   ```cmd
   python tools/http_receiver_server.py --port 8080
   ```
3. Set the endpoint on the board:
   ```
   SERVER https://<subdomain>.trycloudflare.com/upload
   ```
4. Send the simple trigger command (via SMS, COM8, or MQTT):
   ```
   FOTA
   ```
5. The device executes:
   - Derives the binary URL `/nwy_open_app_fota.pkt` and requests it over native HTTPS/TLS.
   - Extracts `X-FOTA-Size`, `X-FOTA-SHA256`, and `X-FOTA-Signature` from HTTP headers to ensure ground-truth synchronization.
   - Streams all chunks directly to NOR flash via `nwy_fota_write()`.
   - Proactively closes connection cleanly at 100% and executes `uECC_verify()`.
   - On signature authentication (`uECC_verify result: 1`), calls `nwy_fota_update(1)` and reboots into the updated firmware.

---

## 6. Dual-Channel MQTT Command Terminal

In addition to SMS and serial console, the device provides real-time dual-channel command and control over cloud MQTT:
- **Broker**: `mqtt.fervidlabs.in:1883`
- **Device Publish / Telemetry Topic**: `sensor/input/N706B/BM688`
- **Device Subscribe / Command Topic**: `sensor/output/N706B/BM688`

Run the interactive terminal:
```cmd
python tools/mqtt_remote_terminal.py
```
Send commands (`STATUS`, `START <LABEL>`, `STOP`, `LIST`, `UPLOAD ALL`, `FOTA`) and receive real-time device logs and alerts.

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
