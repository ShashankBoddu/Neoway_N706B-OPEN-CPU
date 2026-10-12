# Neoway N706B OpenCPU Generic GNSS Location System

Industrial, reliable, and battery-optimized GNSS location subsystem for the **Neoway N706B Cat-1 LTE OpenCPU** module, engineered directly against the official schematic (`SCH_n706b_opencpu.pdf`), hardware user guide, and OpenCPU SDK.

---

## 1. Hardware Architecture & Schematic Mapping

| Signal / Pin Name | Module Pin | Schematic Net / RefDes | Technical Specification |
| :--- | :--- | :--- | :--- |
| **`ANT_GNSS`** | **Pin 2** | `ANT_GNSS` $\rightarrow$ `JP3` | 50 $\Omega$ RF trace to GNSS antenna connector through matching network (`C25`, `R16`, `C26`) and ESD diode `PESD3V3T1BLYL`. |
| **Active Antenna Bias-T** | Power Feed | `VBAT` $\rightarrow$ `R14` (10 $\Omega$) $\rightarrow$ `L2` (47nH) $\rightarrow$ `C21` (100pF) | Feeds 3.8V–4.2V `VBAT` directly to active GNSS antenna coaxial center conductor. |
| **`GNSS_BACKUP`** | **Pin 44** | `GNSSBACKUP` | Internal RTC / Ephemeris memory backup power rail to maintain orbital almanac for **$\le 3\text{s}$ Hot Starts**. |
| **`I2C2_SCL`** | **Pin 67** | `GPIO 10` | Hardware I2C Clock to Puya `P24C128D` EEPROM (4.7k $\Omega$ pull-up `R25` to `VDD_EXT` 1.8V). |
| **`I2C2_SDA`** | **Pin 66** | `GPIO 11` | Hardware I2C Data to Puya `P24C128D` EEPROM (4.7k $\Omega$ pull-up `R26` to `VDD_EXT` 1.8V). |
| **Debug UART** | Pin 38/39 | `DBG_TXD` / `DBG_RXD` | `URT1` at 115200 baud, 8-N-1 for live operational telemetry logging. |

---

## 2. Positioning Capabilities: Native GNSS & Alternatives

### Is this module GNSS or other (for location)?
**Yes, the Neoway N706B-CN-10 features an integrated hardware GNSS receiver silicon core.**
* **Supported Satellite Constellations:**
  * **GPS L1**: 1575.42 $\pm$ 1.023 MHz
  * **BeiDou BDS B1I**: 1559.1 – 1563.1 MHz
  * **GLONASS G1**: 1597.5 – 1605.9 MHz
  * **Galileo E1**: 1575.42 MHz
* **Receiver Specifications:**
  * Tracking Sensitivity: $\le -159$ dBm
  * Acquisition Sensitivity: $\le -145$ dBm
  * Positioning Accuracy: $< 2$ meters (Open Sky)
  * Update Rate: 1 Hz to 10 Hz
  * Protocols: NMEA-0183 (`RMC`, `GGA`, `GSA`, `GSV`, `VTG`, `GLL`, `ZDA`), ICOE Protocol

### Alternative Positioning Methods Available on N706B:
1. **Cellular Base-Station LBS (`nwy_lbs_api.h`):** Cellular triangulation using serving and neighbor Cell IDs, LAC, MCC, and MNC. Operates instantly indoors with no satellite visibility.
2. **Wi-Fi Scan Positioning (`nwy_wifi_api.h`):** The module's `ANT_WCN` (Pin 98) can passively sniff nearby 2.4 GHz Wi-Fi AP BSSID/RSSI for indoor positioning.

---

## 3. Battery Optimization Architecture

A GNSS receiver running continuously consumes **$\approx 45\text{ to }48\text{ mA}$** (typical tracking current at 3.8V). On a battery-operated device (e.g., 2000 mAh Li-ion battery), running continuously will deplete the battery in less than 40 hours.

### The Industrial "Periodic Duty-Cycle Fix" Strategy:
```mermaid
flowchart TD
    A[Deep Sleep / Standby (0.01 mA)] -->|Scheduled Interval (e.g. 15 min)| B[Wake Up Core Application]
    B --> C[Power ON GNSS Receiver (45 mA)]
    C --> D[Search Satellites with Timeout (Max 45s)]
    D -->|Fix Acquired (TTFF ~3-15s)| E[Capture Coordinates & Telemetry]
    D -->|Timeout Expired (Bad Signal)| F[Record Timeout Notice]
    E --> G[Immediately Power OFF GNSS Receiver (0 mA)]
    F --> G
    G --> H[Save Fix to P24C128D EEPROM (CRC-32)]
    H --> I[Transmit Telemetry via LTE / MQTT (Optional)]
    I --> A
```

### Mathematical Battery Life Comparison:
* **Continuous Tracking Mode (1 Hz):**
  $$I_{avg} = 45\text{ mA} \implies \text{Battery Life (2000 mAh)} \approx \frac{2000}{45} \approx \mathbf{44 \text{ hours}} \quad (\approx 1.8 \text{ days})$$
* **Duty-Cycled Mode (1 fix every 15 minutes, TTFF = 5s with Hot Start):**
  $$T_{active} = 5\text{ s} \text{ out of } 900\text{ s} = 0.55\% \text{ duty cycle}$$
  $$I_{avg} = (45\text{ mA} \times 0.0055) + (0.05\text{ mA sleep}) \approx 0.30\text{ mA}$$
  $$\text{Battery Life (2000 mAh)} \approx \frac{2000}{0.30} \approx \mathbf{6,666 \text{ hours}} \quad (\mathbf{> 270 \text{ days / 9 months!}})$$

---

## 4. Software Design & Reliability Principles

1. **Deterministic Mutual Exclusion:** All public APIs and shared state structures are protected by FreeRTOS recursive mutexes (`s_gnss_mutex`) to guarantee thread safety across concurrent OpenCPU tasks.
2. **Bounded Non-Blocking Execution:** Every acquisition routine operates with strict, bounded hardware timeouts (`timeout_sec`), preventing infinite loops, thread freezes, or watchdog resets.
3. **Emergency Power Cut:** Regardless of whether a satellite fix succeeds or times out, the receiver hardware is guaranteed to power down via `nwy_loc_navigation_stop()`.
4. **Persistent Last-Known Position (LKP):** Fix coordinates (Lat/Long, Altitude, Speed, Satellites, HDOP, TTFF) are committed to the onboard `P24C128D` EEPROM at address `0x0300` protected by an IEEE 802.3 CRC-32 checksum. Upon reboot, the system immediately restores the last known location.

---

## 5. File Structure

```
Code/GNSS_Location/
├── inc/
│   ├── gnss_mgr.h          # Generic GNSS manager API definitions & telemetry structs
│   ├── eeprom_24cxx.h      # P24C128D EEPROM storage driver
│   └── i2c_bus.h           # Fast 400 kHz I2C bus driver
├── src/
│   ├── gnss_mgr.c          # GNSS manager implementation & state machine
│   ├── eeprom_24cxx.c      # EEPROM page-chunking & CRC-32 engine
│   ├── i2c_bus.c           # Hardware / Software 400 kHz I2C driver
│   └── main.c              # Test suite & duty-cycle application entry
├── release/
│   ├── nwy_open_app.zip    # Aboot flasher package
│   └── nwy_open_app_fota.pkt# FOTA update package
├── build_GNSS_Location.bat # Automated build script
└── README.md               # Complete documentation
```

---

## 6. Continuous Tracking vs Battery Duty-Cycled Mode

In `src/main.c`, you can toggle between two operational modes via `#define GNSS_CONTINUOUS_TRACKING_MODE`:

```c
#define GNSS_CONTINUOUS_TRACKING_MODE 1  /* 1 = CONTINUOUS live tracking & NMEA stream, 0 = Battery Duty-Cycled */
```

### Mode 1: Continuous Real-Time Tracking (`GNSS_CONTINUOUS_TRACKING_MODE 1` - CURRENT ACTIVE)
* **Behavior:**
  - Receiver is kept permanently powered ON (`nwy_loc_navigation_start()`).
  - Never shuts down or enters duty-cycle sleep.
  - Raw NMEA sentence streaming (`$GNRMC`, `$GNGGA`, `$GPGSV`, `$BDGSV`, `$GPGSA`) is directed live to the debug UART (`URT1` @ 115200 baud).
  - Every 2 seconds, polls telemetry and logs `[LIVE LOCK]` or `[SEARCHING (Sats in view)]`.
  - When locked, automatically commits the latest fix to EEPROM (or RAM cache if on EVB).
* **Ideal for:** Initial bench testing, antenna RF verification, real-time vehicle fleet tracking.

### Mode 2: Battery-Optimized Duty-Cycle (`GNSS_CONTINUOUS_TRACKING_MODE 0`)
* **Behavior:**
  - Wakes up on scheduled interval (e.g., every 60 s or 15 min).
  - Powers ON GNSS receiver and searches for satellites up to `GNSS_FIX_TIMEOUT_SEC` (45 s).
  - As soon as a fix is acquired (or on timeout), **immediately powers OFF the receiver** (0 mA draw).
  - Saves fix to EEPROM with CRC-32.
  - Puts CPU/GNSS to sleep in non-blocking RTOS slices.
* **Ideal for:** Battery-operated asset trackers, pet trackers, solar-powered field sensors.

---

## 7. How to Build and Flash

### 1. Compile the Project:
Double-click or run from command line:
```cmd
e:\projects\DevelopmentLevelCode\Neoway_N706B\Code\GNSS_Location\build_GNSS_Location.bat
```

### 2. Flash to Module:
* **Tool:** Neoway Aboot Tool (`arelease` / Aboot GUI).
* **Package Path:** `Code/GNSS_Location/release/nwy_open_app.zip`.
* **Baudrate:** 115200 on `URT1` (Main Debug Port).

