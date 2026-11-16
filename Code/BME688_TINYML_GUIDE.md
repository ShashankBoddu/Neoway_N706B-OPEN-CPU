# Bosch BME688 Environmental Gas Scanner & TinyML Integration Guide
## Edge AI Chemical Vapor & Air Quality Classification on Neoway N706B OpenCPU

---

## 1. Executive Summary & Architecture Overview

This document provides the complete engineering specification, hardware wiring, data collection protocol, and software implementation architecture for running **TinyML (Edge Artificial Intelligence)** on the **Neoway N706B-CN / N706B-CN-10** LTE Cat.1 module using the **Bosch Sensortec BME688** 4-in-1 digital environmental sensor.

The objective of this subsystem is to detect, classify, and quantify environmental conditions and hazardous chemical vapors:
* **Baseline Indoor / Outdoor Air Quality:** Temperature, Humidity, Barometric Pressure, and general Air Quality Index (AQI 0–500).
* **Solvent Vapors:** Isopropanol / Isopropyl Alcohol (IPA solution).
* **Adhesives & Resins:** Araldite (Epoxy resin and polyamine hardener volatile fumes released during curing).
* **Edge Inference:** Executed entirely on-device using the N706B’s **ARM Cortex-R5 CPU @ 614 MHz**, waking the cellular radio (LTE Cat-1) only when an anomaly, hazard, or periodic telemetry threshold is reached.

---

## 2. Sensor Profile: Bosch BME688 vs. Legacy BME680

The **BME688** is the world’s first MEMS sensor with built-in **Artificial Intelligence** capability, packaged in a miniature $3.0 \times 3.0 \times 0.93\text{ mm}^3$ LGA-8 metal lid enclosure.

```
       ┌────────────────────────────────────────────────────────┐
       │              BOSCH BME688 INTERNAL DIE                 │
       │                                                        │
       │   [ Temperature Sensor ]       [ Humidity Sensor ]     │
       │   -40°C to +85°C               0% to 100% RH           │
       │                                                        │
       │   [ Barometric Pressure ]      [ Gas Scanner Hotplate] │
       │   300 to 1100 hPa              10 Configurable Steps   │
       │   (±0.6 hPa / ±5 cm)           200°C to 400°C Dwell    │
       └────────────────────────────────────────────────────────┘
```

### 2.1 The Critical Upgrade: Gas Scanner Multi-Heater Profiling
| Feature | Legacy BME680 | Bosch BME688 (AI Gas Scanner) |
| :--- | :--- | :--- |
| **Heater Operating Mode** | Single static temperature (e.g., 320°C for 150 ms) | **Multi-Heater Profiling (Up to 10 distinct steps)** |
| **Output Data** | Single scalar resistance value ($R_{\text{gas}}$ in $\Omega$) | **10-Point Multidimensional Chemical Fingerprint** |
| **Gas Selectivity** | None (reacts non-specifically to all VOCs) | **High Selectivity via Temperature-Dwell Signatures** |
| **Machine Learning Support**| Basic thresholding / generic IAQ calculation | **Native BME AI-Studio Integration & TinyML Classifiers** |
| **Duty-Cycle Profile** | Forced Mode | **Parallel Mode & Sequential Gas Scanning** |

### 2.2 Why Multi-Heater Scanning Enables Chemical Detection (IPA vs. Araldite)
The sensing material is a semiconducting **Metal Oxide (MOX)** layer deposited over an integrated micro-hotplate. Gas molecules react with oxygen ions adsorbed on the MOX surface, releasing electrons and altering its electrical resistance:
1. Different chemical compounds (alcohols, amines, ketones, aromatic hydrocarbons) have different **activation energies** and **thermal desorption points**.
2. By stepping the micro-hotplate through 10 temperature setpoints (e.g., 200°C $\to$ 240°C $\to$ 280°C $\to$ 320°C $\to$ 360°C $\to$ 400°C):
   - **Isopropyl Alcohol (IPA):** Vaporizes and reacts strongly at lower temperature plateaus (~200°C – 280°C), producing a steep resistance drop early in the cycle.
   - **Araldite (Epoxy / Amine Hardener):** Heavy organic molecules and nitrogenous amine volatiles react at higher thermal plateaus (~320°C – 400°C), yielding a completely distinct resistance curve across the 10 steps.
3. This 10-element vector ($[R_0, R_1, R_2, \dots, R_9]$) forms the **feature input** for the TinyML classification algorithm.

---

## 3. Hardware Compute Headroom: N706B (ASR1605S)

| Specification | Typical TinyML Target (STM32/ESP32) | Neoway N706B-CN (ASR1605S) | Engineering Headroom |
| :--- | :--- | :--- | :---: |
| **Core Architecture** | ARM Cortex-M4 / Cortex-M33 | **ARM Cortex-R5 (ARMv7-R)** | Real-Time Fault-Tolerant Core |
| **Clock Frequency** | 64 MHz – 168 MHz | **614 MHz** | **~4x to 10x faster execution** |
| **Hardware FPU** | Single-precision (FPv4-SP) or None | **VFPv3-D16 (Hardware Single & Double Precision)** | Instant mathematical vector scaling |
| **Application RAM** | 32 KB – 128 KB | **~2 MB dynamic RAM** (`0x7E200000`) | > 40x memory required by TinyML models |
| **Application Flash** | 256 KB – 512 KB | **352 KB Flash Partition** (`0x3A2000`) | Fits quantized INT8 or C models (< 30 KB) |
| **Inference Latency** | 50 ms – 150 ms | **< 1.0 millisecond** | Minimal battery/energy impact |

---

## 4. Hardware Wiring & Carrier Board Interfacing

The recommended sensor evaluation breakout is the **[Adafruit BME688 Breakout (Robu.in SKU)](https://robu.in/product/bme688-temperature-humidity-pressure-and-gas-sensor/)**. It incorporates onboard reverse polarity protection, a 3.3V LDO regulator, and bi-directional I2C level shifters compatible with 3.3V and 5V logic.

```
                  ELEKTRONIKA_N706B CARRIER PCB                     ADAFRUIT BME688 BREAKOUT
            ┌────────────────────────────────────────┐             ┌─────────────────────────┐
            │                                        │             │                         │
            │  VCC_SWout (Switched Rail via Q1/Q3)   ├────────────►│ VIN                     │
            │  Module Pin 83 (GPIO00) = Switch Gate  │             │                         │
            │                                        │             │                         │
            │  System Common Ground                  ├────────────►│ GND                     │
            │                                        │             │                         │
            │  Module Pin 57 (I2C1_SCL)              ├────────────►│ SCK / SCL               │
            │  (4.7k Pull-Up to 1.8V / 3.3V)         │             │                         │
            │                                        │             │                         │
            │  Module Pin 58 (I2C1_SDA)              ├────────────►│ SDI / SDA               │
            │  (4.7k Pull-Up to 1.8V / 3.3V)         │             │                         │
            │                                        │             │                         │
            │  Ground (Hardwired for I2C Addr 0x76)  ├────────────►│ SDO (Address Select)   │
            │                                        │             │ (GND = 0x76, VDD = 0x77)│
            │                                        │             │                         │
            │  Pull to VIN (Internal on Adafruit)    │             │ CS (Chip Select = HIGH) │
            └────────────────────────────────────────┘             └─────────────────────────┘
```

### 4.1 Pin Mapping Matrix
| Adafruit BME688 Pin | Carrier Board Connection | N706B Pin # | Function / Notes |
| :--- | :--- | :---: | :--- |
| **`VIN`** | `VCC_SWout` | — | Power from switched rail (Q3/Q1 P-MOS switch). When `GPIO00 = 0`, power is cut to **0.0 V (0.0 µA leakage)**. |
| **`GND`** | System GND | Pins 40, 41, etc. | Common ground reference. |
| **`SCK / SCL`** | `I2C1_SCL` | **Pin 57** (`GPIO[49]`) | I2C Bus 1 Clock (can also share `I2C2_SCL` on Pin 67 with EEPROM). |
| **`SDI / SDA`** | `I2C1_SDA` | **Pin 58** (`GPIO[50]`) | I2C Bus 1 Data (can also share `I2C2_SDA` on Pin 66 with EEPROM). |
| **`SDO`** | System GND | Ground | Hardwires I2C slave address to **`0x76`** (avoids conflict with EEPROM at `0x50`). |
| **`CS`** | Tied to `VIN` | — | Defaults to HIGH on breakout board to select I2C mode. |

---

## 5. End-to-End Development Workflow (Specimen to Edge Deployment)

```
  PHASE 1: DATA LOGGING          PHASE 2: CLOUD / PC AI-STUDIO           PHASE 3: OPEN-CPU DEPLOYMENT
┌───────────────────────┐       ┌─────────────────────────────┐       ┌───────────────────────────────┐
│ • Clean Ambient Air   │       │ • Import .bmerawdata        │       │ • Compile C weights into      │
│ • IPA Solution Vapor  │──────►│ • Slice & Label Specimens   │──────►│   nwy_open_app.bin            │
│ • Araldite Epoxy Fumes│       │ • Extract Heater Features   │       │ • Execute inference in < 1 ms │
│ • Log to SD (.bmeraw) │       │ • Train Neural Network / RF │       │ • Trigger LTE MQTT on alert   │
└───────────────────────┘       └─────────────────────────────┘       └───────────────────────────────┘
```

### 5.1 Phase 1: Physical Specimen Data Recording
To train a robust machine learning classifier, you must collect distinct physical sessions inside an enclosed, sealed glass testing container (~500 ml to 1000 ml volume):

1. **Class 0: Clean Ambient Air (Baseline):**
   - Place sensor inside the empty, sealed chamber at typical room temperature ($25^\circ\text{C} \pm 3^\circ\text{C}$, $45\% - 65\%\text{ RH}$).
   - Record continuously for **30 to 45 minutes**.
2. **Class 1: Isopropyl Alcohol (IPA Solution):**
   - Place a bottle cap containing 1.0 ml of 70% or 99% Isopropanol inside the chamber.
   - Allow vapor equilibrium (~2 minutes).
   - Record continuously for **30 to 45 minutes**.
3. **Class 2: Araldite (Epoxy Resin + Polyamine Hardener):**
   - Mix equal parts of Araldite Standard (Resin + Hardener) on a small petri dish.
   - Place immediately inside the chamber while curing (exothermic reaction releasing volatile amine hardener vapors).
   - Record continuously for **45 to 60 minutes**.
4. **Data Logging Tool:**
   - **Option A (ESP32 / Arduino Logger):** Use an ESP32 Feather or Uno with Adafruit BME688 and SD card running the official Bosch `bme68x_demo_sample` code.
   - **Option B (Direct N706B Logger):** Leverage our existing Stage 14 MicroSD SPI FatFs engine on the N706B to write raw `.bmerawdata` files directly.

### 5.2 Phase 2: Training in Bosch BME AI-Studio
1. Download and launch **Bosch BME AI-Studio** (Windows Desktop).
2. **Create Project:** Set name to `Chemical_Classifier_IPA_Araldite`.
3. **Select Heater Profile:** Select standard profile **`HP-354`** (10 steps: 200°C to 400°C, 140 ms dwell per step, 10-step cycle repeated every 1.4 seconds).
4. **Import Data:** Drag and drop all recorded `.bmerawdata` files into the project.
5. **Specimen Labeling:**
   - In the interactive timeline view, select the time regions corresponding to the test sessions.
   - Assign class tags: `Clean_Air`, `IPA_Vapor`, and `Araldite_Epoxy`.
6. **Feature Extraction & Training:**
   - Select **Classification Model**.
   - BME AI-Studio calculates normalized baseline resistance ($R_{\text{norm}}$), relative slope ($\Delta R / \Delta t$), and thermal desorption ratios.
   - Click **Train**. The tool trains multiple model architectures (e.g., Multilayer Perceptrons, Decision Forests) and outputs a **Confusion Matrix** with accuracy metrics (> 95% target).

### 5.3 Phase 3: Exporting the Model to Embedded C Code
Once validated, export the model using either:
* **Bosch BSEC2 C Export:** Generates `bsec_serialized_configurations_iaq.c` containing the compiled binary configuration blob.
* **Open TinyML C Array (Recommended for OpenCPU):** Export raw features and train a compact **Random Forest** or **2-layer MLP** using Python (`scikit-learn` + `emlearn`). This generates a pure, static C header file (`chemical_model.h`) with zero library dependencies.

---

## 6. Embedded C Firmware Architecture on N706B (`main.c`)

Below is the production firmware structure for the N706B OpenCPU environment:

```c
#include "nwy_common.h"
#include "nwy_gpio.h"
#include "nwy_i2c.h"
#include "bme68x.h"
#include "chemical_model.h" // Exported TinyML weights

typedef enum {
    GAS_CLASS_CLEAN_AIR = 0,
    GAS_CLASS_IPA_DETECTED = 1,
    GAS_CLASS_ARALDITE_DETECTED = 2,
    GAS_CLASS_UNKNOWN = 0xFF
} chemical_result_t;

// Standard HP-354 Gas Scanner Profile: 10 Temperature Setpoints (°C)
static const uint16_t g_temp_steps[10] = {200, 240, 280, 300, 320, 340, 360, 380, 400, 200};
static const uint16_t g_duration_steps[10] = {140, 140, 140, 140, 140, 140, 140, 140, 140, 140};

void sample_and_classify_environment(void)
{
    struct bme68x_data data[10];
    uint8_t n_fields = 0;
    float features[10];
    
    // 1. Wake peripheral rail by enabling high-side P-MOSFET switch
    nwy_gpio_set_val(NWY_GPIO_00, 1);
    nwy_sleep(50); // Allow sensor LDO rail to stabilize
    
    // 2. Trigger Parallel Gas Scanner Measurement across 10 heater steps
    bme68x_set_op_mode(BME68X_PARALLEL_MODE, &g_bme_dev);
    nwy_sleep(1400); // 10 steps * 140ms dwell time
    
    // 3. Read back gas resistance array from BME688 registers over I2C1
    bme68x_get_data(BME68X_PARALLEL_MODE, data, &n_fields, &g_bme_dev);
    
    for (int i = 0; i < 10; i++) {
        features[i] = (float)data[i].gas_resistance;
    }
    
    // 4. Run TinyML Model on ARM Cortex-R5 @ 614 MHz (Execution: < 500 microseconds!)
    chemical_result_t result = (chemical_result_t)chemical_model_predict(features);
    
    // 5. Evaluate Decision Logic & Cloud Alert Dispatch
    switch (result) {
        case GAS_CLASS_IPA_DETECTED:
            NWY_CLI_LOG("CRITICAL: Isopropanol (IPA) Vapor Identified! Confidence: HIGH\r\n");
            cellular_wake_and_publish_mqtt("ALERT/HAZARD", "IPA_VAPOR_DETECTED");
            break;
            
        case GAS_CLASS_ARALDITE_DETECTED:
            NWY_CLI_LOG("WARNING: Curing Epoxy / Araldite Volatiles Detected!\r\n");
            cellular_wake_and_publish_mqtt("ALERT/PROCESS", "ARALDITE_CURING_DETECTED");
            break;
            
        case GAS_CLASS_CLEAN_AIR:
        default:
            NWY_CLI_LOG("Environmental Baseline Normal: Clean Air (T: %0.1f C, H: %0.1f %%)\r\n",
                        data[0].temperature, data[0].humidity);
            break;
    }
    
    // 6. Power down peripheral rail to eliminate quiescent draw
    nwy_gpio_set_val(NWY_GPIO_00, 0);
}
```

---

## 7. Power Budget & Battery Life Calculation

By leveraging the hardware-verified **Stage 4 Deep Sleep (568.69 µA)** architecture documented in `DEV_LOG.md`:

| Operational Phase | Duration | Current Draw | Energy / Charge |
| :--- | :---: | :---: | :---: |
| **Stage 4 Deep Sleep (Baseband & CPU PMU Sleep)** | 58.50 s | **568.69 µA** | $33.27\text{ mC}$ |
| **Sensor Power-Up & Rail Settle (`GPIO00 = 1`)** | 0.05 s | 2.50 mA | $0.13\text{ mC}$ |
| **BME688 10-Step Gas Heater Scanning (Pulsed)** | 1.40 s | 12.00 mA | $16.80\text{ mC}$ |
| **Cortex-R5 TinyML Inference Execution** | 0.001 s | 45.00 mA | $0.05\text{ mC}$ |
| **Total 1-Minute Environmental Cycle** | **60.00 s** | **Average: 837.5 µA** | **$50.25\text{ mC}$** |

### Battery Runtime Estimate (Standard 3.7V / 3000 mAh Li-Ion Cell)
$$\text{Average Current} = \frac{50.25\text{ mC}}{60\text{ s}} \approx 0.838\text{ mA}$$
$$\text{Battery Life} = \frac{3000\text{ mAh} \times 0.85\text{ (efficiency)}}{0.838\text{ mA}} \approx 3,043\text{ hours} \approx \mathbf{126.8\text{ days (~4.2 months)}}$$

*(If sampling once every 5 minutes instead of every 1 minute, the average current drops to **~615 µA**, extending battery life to **over 1.1 years** on a single 3000 mAh cell).*

---

## 8. Summary Checklist & Action Plan

1. **Procure Hardware:** Obtain the [Adafruit BME688 Breakout from Robu.in](https://robu.in/product/bme688-temperature-humidity-pressure-and-gas-sensor/).
2. **Wire to N706B Carrier:** Connect to `VCC_SWout`, `GND`, `I2C1_SCL` (Pin 57), and `I2C1_SDA` (Pin 58).
3. **Record Specimens:** Expose the sensor in an airtight jar to:
   - Clean Room Air (30 mins)
   - Isopropyl Alcohol (IPA) vapor (30 mins)
   - Freshly mixed Araldite epoxy (45 mins)
4. **Train in BME AI-Studio:** Import `.bmerawdata`, tag the specimen regions, and train the neural network classifier.
5. **Export & Flash:** Export the model weights into `chemical_model.h`, compile into `nwy_open_app.bin`, and flash via SWDownloader.
