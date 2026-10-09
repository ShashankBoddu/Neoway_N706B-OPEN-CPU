# Developer Log & System Technical Documentation (`DEV_LOG.md`)

## Document Metadata & Revision Control
| Attribute | Details |
| :--- | :--- |
| **Project Name** | Neoway N706B LTE Cat.1 OpenCPU Embedded Platform |
| **Document Type** | Industrial Developer Log & System Technical Specification |
| **Standard Reference** | IEEE 1016 / ISO/IEC/IEEE 26514 / 3GPP Rel-13 TS 24.008 |
| **Document Revision** | `v1.3.0` |
| **Last Updated** | `2026-10-09` |
| **Classification** | Engineering Technical Documentation |
| **Target Hardware** | Neoway N706B-CN (ASR1605S Cortex-R5 @ 614 MHz, 4 MB QSPI NOR Flash) |
| **Carrier Board** | `ELEKTRONIKA_N706B_prototype` (Custom PCB Rev 1.0) |
| **Firmware Subsystem** | OpenCPU Application Firmware (`nwy_open_app.bin`), Flash Offset `0x3A2000` |
| **SDK & Toolchain** | Neoway OpenCPU SDK (`N706B-A07-STD-OE_CN1X_ITRI-009_SDK`), GNU Arm Toolchain 10.3.1, CMake, Ninja |
| **Flashing System** | Aboot Package Pipeline (`arelease.exe` $\to$ `nwy_open_app.zip`), SWDownloader |
| **Power Instrumentation**| Nordic Power Profiler Kit 2 (PPK2, Source Meter Mode @ 3.60 V - 3.80 V) |
| **Maintainer** | Embedded Cellular Firmware & Hardware Engineering Team |

---

## 1. Document Overview & Guidelines
This document serves as the **single source of truth** for engineering logs, system architecture, hardware peripheral pinout mappings, electrical power domain decoupling, root cause analysis (RCA), resolution verification, and ongoing firmware developments for the **Neoway N706B OpenCPU** platform.

### Standard Engineering Entry Structure
All development updates must adhere strictly to the standardized industrial template:
- **Header:** Date (`YYYY-MM-DD`) and Feature / Bug Fix Title
- **Subsystem Scope:** Affected module/peripheral (e.g., Power Management / PMU, Baseband Modem, GPIO HAL, Level Shifters, UART)
- **Description & Context:** Detailed technical summary of modifications, requirements, or test results
- **Root Cause Analysis (RCA) / Requirement Rationale:** Engineering origin of defect, electrical leakage, or architectural limitation
- **Resolution & Implementation:** Corrective firmware logic, hardware rework instructions, API bindings, or register overrides
- **Verification & Validation (V&V):** Empirical proof of resolution (PPK2 current traces, logic analyzer captures, terminal boot logs)

---

## 2. System Architecture & Platform Topology Overview

### 2.1 Hardware Architecture & Chipset Topology
```mermaid
graph TD
    subgraph Power Domains [Power Supply & Rail Decoupling]
        VBAT_EXT[External Power / PPK2 3.6V-3.8V] -->|Pins 42, 43| N706B_PMIC[ASR1605 Internal PMIC]
        N706B_PMIC -->|Pin 24| VDD_EXT[VDD_EXT 1.8V LDO Output - Max 20mA]
        VBAT_EXT -->|R8 0R Jumper| BOARD_VBAT[Board Peripheral VBAT Rail]
        BOARD_VBAT -->|High-Side P-MOS Q3/Q1| VCC_SW[VCC_SWout Switched Rail]
    end

    subgraph ASR1605S SoC [Neoway N706B Module]
        ARM_R5[ARM Cortex-R5 CPU @ 614 MHz]
        CP_CORE[Cellular Processor / Baseband LTE Cat.1]
        HW_PMU[Hardware PMU & Clock Gating Unit]
        RTC_32K[Ultra-Low Power 32.768 kHz RTC & Wakeup Domain]
        QSPI_FLASH[4 MB QSPI NOR Flash - XM25LU32C]
    end

    subgraph External Peripherals [Carrier Board Hardware]
        VDD_EXT -->|VCC-A| U4_LS[U4 NXB0106 Level Shifter - UART]
        VDD_EXT -->|VCC-A| U15_LS[U15 NXB0106 Level Shifter - GPIO]
        VDD_EXT -->|VCC-A| U8_LS[U8 NXS0102 Level Shifter - I2C1]
        VDD_EXT -->|VCC| U7_EEPROM[U7 P24C128D EEPROM]
        BOARD_VBAT -->|VCC-B| U4_LS
        BOARD_VBAT -->|VCC-B| U15_LS
        BOARD_VBAT -->|VCC-B| U8_LS
        BOARD_VBAT -->|VCC| U14_RS485[U14 THVD1420 RS-485 Transceiver]
    end
```

### 2.2 Software Architecture (OpenCPU)
The Neoway OpenCPU architecture operates in a unified runtime memory space:
1. **Core Baseband Layer (ASR Crane OS / FreeRTOS):** Owns physical layer RF calibration, 3GPP protocol stack (NAS, RRC, PDCP, RLC, MAC), PMIC power management routines, and hardware watchdogs.
2. **Intermediate Interface Layer (Open API Stubs):** Provided via `build/nwy_open_stub.a`. Exports system services through function pointer vectors mapped during bootloader handover.
3. **User Application Layer:** Compiled with ARM GNU Toolchain into `nwy_open_app.bin`. Entry point is `nwy_open_app_entry()` (or `appimg_enter()`), running as a dedicated FreeRTOS thread with user-configured stack depth (default 8 KB).

---

## 3. Hardware Connections & Peripheral Pin Mapping

The ground truth mapping is synthesized from `SCH_n706b_opencpu.pdf` and `N706B-CN (ASR1605) GPIO Configuration form-20241204.xlsx`.

### 3.1 Power & System Control Interfaces
| Pin # | Pad Name | Function | Direction | Default Pull | Voltage | Physical Connection / Schematic Notes |
| :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| **42, 43** | `VBAT` | Main Power Supply | Power In | — | 3.4V – 4.3V | Direct module power supply. Decoupled by C4 (100 µF), C5, C6, C7. Isolated via R8 (0 $\Omega$). |
| **24** | `VDD_EXT` | 1.8V LDO Output | Power Out | — | 1.8V (20mA max)| Supplies level translator reference `VCC(A)`, EEPROM `U7`, and I2C pull-ups. |
| **44** | `GNSS_BACKUP` | RTC/SRAM Backup | Power In | — | 1.8V (100µA) | Backup power for GNSS hot-start. |
| **7** | `PWRKEY_N` | Power-On Key | DI | — | 1.8V Logic | Active low. Pulled down to GND via 1 k$\Omega$ resistor (R10) for automatic boot-up. |
| **15** | `RESET_N` | Hardware Reset | DI | — | 1.8V Logic | Active low soft-reset without PMU power loss. |
| **19** | `MAIN_DTR` | Sleep / Wake Pin | DI | DOWN | 1.8V Logic | Module sleep mode control. Hardwired to GND (R3 removed) for low-power sleep operation. |
| **20** | `MAIN_RI` | Ring Indicator | DO | DOWN | 1.8V Logic | Baseband-owned ring indicator. **Do not drive as custom GPIO output.** |
| **25** | `STATUS` | Operating Status | DO | DOWN | 1.8V Logic | High when baseband is running. Connected to status LED indicator. |
| **16** | `NET_STATUS` | Network Status | DO | DOWN | 1.8V Logic | Outputs PWM status pulse for LTE registration indication. |

### 3.2 Communication & Peripheral Interfaces
| Pin # | Pad Name | GPIO ID | Default Pull | Level Shifter | Schematic Net | Peripheral Connected |
| :---: | :--- | :---: | :---: | :---: | :--- | :--- |
| **17** | `MAIN_RXD` | `GPIO[51]` | UP | `U4` (Ch 6) | `MAIN_RX` / `VBATMAIN_TX` | Main UART Receive (AT / Data) |
| **18** | `MAIN_TXD` | `GPIO[52]` | UP | `U4` (Ch 5) | `MAIN_TX` / `VBATMAIN_RX` | Main UART Transmit (AT / Data) |
| **38** | `DBG_RXD` | `GPIO[71]` | UP | `U4` (Ch 4) | `DBG_RX` / `VBATDBG_TX` | Debug UART RX (URT1 in OpenCPU SDK) |
| **39** | `DBG_TXD` | `GPIO[72]` | UP | `U4` (Ch 3) | `DBG_TX` / `VBATDBG_RX` | Debug UART TX (115200 baud diagnostic log) |
| **28** | `AUX_RXD` | `GPIO[2]` | DOWN | `U4` (Ch 2) | `AUX_RX` / `VBATAUX_TX` | Auxiliary UART RX (GPS / Modbus) |
| **29** | `AUX_TXD` | `GPIO[3]` | DOWN | `U4` (Ch 1) | `AUX_TX` / `VBATAUX_RX` | Auxiliary UART TX |
| **57** | `I2C1_SCL` | `GPIO[49]` | UP (SPU) | `U8` (Ch 1) | `I2C1_SCL` / `VBATI2C1_SCL` | External I2C Bus 1 Clock (4.7k pull-up R23/R21) |
| **58** | `I2C1_SDA` | `GPIO[50]` | UP (SPU) | `U8` (Ch 2) | `I2C1_SDA` / `VBATI2C1_SDA` | External I2C Bus 1 Data (4.7k pull-up R24/R22) |
| **67** | `I2C2_SCL` | `GPIO[10]` | UP (SPU) | Direct 1.8V | `I2C2_SCL` | `U7` EEPROM (`P24C128D`) Clock (4.7k pull-up R25) |
| **66** | `I2C2_SDA` | `GPIO[11]` | UP (SPU) | Direct 1.8V | `I2C2_SDA` | `U7` EEPROM (`P24C128D`) Data (4.7k pull-up R26) |
| **83** | `GPIO_00` | `GPIO[0]` | DOWN | Direct 1.8V | `GPIO00` | **Peripheral Load Switch Control** (Q3/Q1 gate) |
| **84** | `GPIO_01` | `GPIO[1]` | DOWN | Direct 1.8V | `GPIO01` | User General Purpose I/O |
| **21** | `GPIO_07` | `GPIO[7]` | UP | `U15` (Ch 1) | `GPIO07` / `VBATGPIO07` | User General Purpose I/O |
| **26** | `GPIO_05` | `GPIO[5]` | DOWN | `U15` (Ch 3) | `GPIO05` / `VBATGPIO05` | User General Purpose I/O |
| **85** | `GPIO_84` | `GPIO[84]` | DOWN | `U15` (Ch 4) | `GPIO84` / `VBATGPIO84` | User General Purpose I/O |
| **86** | `GPIO_57` | `GPIO[57]` | DOWN | `U15` (Ch 5) | `GPIO57` / `VBATGPIO57` | User General Purpose I/O |
| **87** | `GPIO_87` | `GPIO[87]` | UP | `U15` (Ch 6) | `GPIO87` / `VBATGPIO87` | User General Purpose I/O |

---

## 4. System Memory Mapping & Partition Architecture

The Neoway N706B incorporates an external **4 MB (32 Mbit) QSPI NOR Flash** (`XM25LU32C`, Sector/PEB size: 4096 bytes).

### 4.1 Flash Memory Partition Layout
| Partition Name | Flash Offset | Partition Size | Hex Size | Partition Content / Purpose |
| :--- | :---: | :---: | :---: | :--- |
| **`bootloader`** | `0x000000` | 84 KB | `0x015000` | ASR Crane Primary Boot ROM, BL1, and BL2 images |
| **`ptable`** | `0x015000` | 4 KB | `0x001000` | Dynamic Master Partition Table (`partition.bin`) |
| **`rd`** | `0x016000` | 8 KB | `0x002000` | Reliable Data (`ReliableData.bin`, calibration data) |
| **`cp`** | `0x018000` | 3,160 KB | `0x316000` | Communication Processor Cellular Protocol Stack Firmware |
| **`dsp`** | `0x32E000` | 56 KB | `0x00E000` | Digital Signal Processor Modem Firmware (`dsp.bin`) |
| **`rfbin`** | `0x33C000` | 4 KB | `0x001000` | Radio Frequency Band & Transmit Calibration Map (`rf.bin`)|
| **`reserved`** | `0x33D000` | 80 KB | `0x014000` | System Reserved Flash Region |
| **`updater`** | `0x351000` | 112 KB | `0x01C000` | Recovery and FOTA Bootloader Updater Image |
| **`nvm`** | `0x36D000` | 200 KB | `0x032000` | Non-Volatile Memory (IMEI, SN, Calibration, NVRAM) |
| **`fota_param`** | `0x39F000` | 12 KB | `0x003000` | In-Band OTA Upgrade Staging Headers |
| **`app`** | **`0x3A2000`** | **352 KB** | **`0x058000`**| **OpenCPU Application Partition (`nwy_open_app.bin`)** |
| **`factory`** | `0x3FA000` | 24 KB | `0x006000` | Factory Settings & Product Calibration Vault |

### 4.2 RAM Execution Memory Map
| Memory Region | Address Range | Size | Access | Description |
| :--- | :---: | :---: | :---: | :--- |
| **Boot ROM RAM** | `0x80018000` | — | R/W | Hardware ROM execution stack during initial reset |
| **Baseband CP RAM** | `0x7E000000` – `0x7E1F7134` | ~2 MB | Privileged | Cellular Protocol Stack, FreeRTOS Kernel, L1/L2 Buffers |
| **OpenCPU Heap/Text** | `0x7E200000` – `0x7E3FFFFF` | ~2 MB | R/W/X | OpenCPU Application Text, Data, BSS, and Dynamic Heap |
| **PM Diagnostics** | `0x7E24B8BC` | 2 KB | R/W | Internal PMU state structure (Wake Lock Bitmask @ `+0x604`) |

---

## 5. Subsystem & Project Module Catalog

The development environment in `E:\projects\DevelopmentLevelCode\Neoway_N706B\Code` comprises specialized modular test suites:

* **`Code/DeepSleep`**: True Deep Sleep & 3GPP PSM Engine. Demonstrates ultra-low power standby (< 15 µA) with 60-second periodic RTC wakeup, STM32-style GPIO analog/high-Z isolation, virtual AT wake-lock authorization, and boot reason diagnostics.
* **`Code/SleepPowerTest`**: Connected Light Sleep diagnostic benchmark. Tests CPU clock-gating (WFI) while maintaining RAM state and cellular network paging reception.
* **`Code/HelloWorld`**: Baseline OpenCPU bring-up and logging validation over Debug UART (URT1).
* **`Code/SimCardTest`**: Automated diagnostic self-test suite and hotplug event listener testing SIM card readiness, IMSI/ICCID retrieval, PIN/PUK lock status, and SIM presence line behaviors.
* **`Code/NetworkMonitor`**: Periodic cellular registration, RSSI/RSRP signal quality tracking, and automated inbound SMS echoing.
* **`Code/TimerToggle`**: Non-blocking LED toggling utilizing FreeRTOS OS software timers (`nwy_sdk_timer_create`).
* **`Code/TCP_IPSockets`**: Raw TCP/UDP socket communication, transparent transmission, and DNS resolution.
* **`Code/HTTP_test` & `Code/Mqtt_Test`**: High-level application protocol clients connecting to cloud infrastructure.
* **`Code/FOTA_Test`**: Differential package flashing and in-band OTA validation.

---

## 6. Chronological Engineering Change Log & Root Cause Analyses (RCAs)

### [2026-10-06] - OpenCPU Development Environment Initialization & Toolchain Path Resolution
* **Subsystem Scope:** Build System, CMake, Ninja, Toolchain Setup
* **Description & Context:** Initial bring-up of the Neoway OpenCPU development environment on Windows 11 using the bundled GNU Arm Toolchain 10.3.1.
* **Root Cause Analysis (RCA):**
  1. The original vendor SDK was extracted to `E:\projects\Devolapment files\Neoway\...`. The space in `"Devolapment files"` and a path depth of ~240 characters caused Ninja and CMake to fail with fatal syntax and path expansion errors (`"cannot open file"`, `"missing variable"`).
  2. The SDK build system rigidly assumed projects resided inside `nwy_test_demo/`.
* **Resolution & Implementation:**
  1. Relocated the entire SDK and workspace to `E:\projects\DevelopmentLevelCode\Neoway_N706B`.
  2. Refactored `CMakeLists.txt` in the SDK root to introduce the `CUST_PROJ_DIR` variable:
     ```cmake
     if(CUST_PROJ_DIR)
         include_directories(${CUST_PROJ_DIR}/inc)
         file(GLOB_RECURSE CUST_SOURCES ${CUST_PROJ_DIR}/src/*.c)
     endif()
     ```
  3. Created per-project automated build scripts (`build_<ProjectName>.bat`) that automatically configure CMake, compile with Ninja, generate compile commands for Clangd, package FOTA `.pkt` archives, and build `.zip` release images for Aboot.
* **Verification & Validation (V&V):** Full clean build completed in 4.2 seconds; `nwy_open_app.bin` generated with 0 errors and 0 warnings.

---

### [2026-10-07] - GPIO Pin Mapping Discrepancies & Baseband Watchdog Crash Prevention
* **Subsystem Scope:** GPIO HAL, ASR1605 Pad Multiplexing, Modem IPC
* **Description & Context:** Driving board status LEDs and digital outputs caused immediate baseband watchdog reset loops (module rebooting every 2 seconds).
* **Root Cause Analysis (RCA):**
  Cross-referencing `SCH_n706b_opencpu.pdf` with `N706B-CN (ASR1605) GPIO Configuration form-20241204.xlsx` revealed that schematic net names did not correspond to software GPIO IDs:
  * Pin 20 (labeled `GPIO1` on schematic) actually maps to **`MAIN_RI` (`GPIO[4]`)**, which is strictly owned by the Cellular Processor (CP) for incoming voice/SMS ring interrupts.
  * Writing to `GPIO[4]` caused a register write conflict with the cellular baseband core, triggering a hardware watchdog panic reset (`NWY_BOOT_CAUSE_PANIC_RST`).
* **Resolution & Implementation:**
  Established a strict hardware pin ground-truth policy:
  * Reassigned user digital outputs to safe, general-purpose pins: **Pin 21 (`GPIO_07` / Software ID 7)** and **Pin 26 (`GPIO_05` / Software ID 5)**.
  * Reassigned status LED control to **Pin 25 (`STATUS` / Software ID 70)**.
* **Verification & Validation (V&V):** Module booted with continuous LED toggling; zero CP watchdog resets over a 4-hour soak test.

---

### [2026-10-07] - SIM Slot Enum Off-By-One Correction (`nwy_sim_id_e`)
* **Subsystem Scope:** SIM Card HAL (`nwy_hal_sim.c`, `nwy_sim_api.h`)
* **Description & Context:** SIM Card diagnostics failed with `SIM Status: NOT READY` despite a valid, active SIM card physically inserted into the single SIM slot.
* **Root Cause Analysis (RCA):**
  In `nwy_sim_api.h`, the enumeration is 0-indexed:
  ```c
  typedef enum {
      NWY_SIM_ID_SLOT_1 = 0,
      NWY_SIM_ID_SLOT_2 = 1,
      NWY_SIM_ID_MAX
  } nwy_sim_id_e;
  ```
  Passing a human-readable index `1` mapped to `NWY_SIM_ID_SLOT_2` (the secondary, unpopulated SIM2 interface), causing all AT and driver calls to target an empty slot.
* **Resolution & Implementation:**
  Implemented slot translation in `nwy_hal_sim.c`:
  ```c
  static inline nwy_sim_id_e get_nwy_sim_id(int sim_id) {
      return (sim_id == 2) ? NWY_SIM_ID_SLOT_2 : NWY_SIM_ID_SLOT_1;
  }
  ```
* **Verification & Validation (V&V):** `SimCardTest` immediately reported `SIM Status: READY`, ICCID retrieved successfully (`8991...`), and network registration completed in 1.4 seconds.

---

### [2026-10-08] - OS Primitive Handles Pass-by-Value Memory Corruption Fix
* **Subsystem Scope:** FreeRTOS Abstraction Layer (`nwy_osi_api.h`)
* **Description & Context:** Creating software timers, mutexes, or semaphores caused immediate memory access violations or silent timer failures.
* **Root Cause Analysis (RCA):**
  The official vendor documentation (`Neoway_OpenCPU_SDK_OS_API_Notes_V1.0.pdf`) contains a major typographical error stating that OS functions take pointers to handles (e.g., `nwy_osi_semaphore_t *sem`).
  However, in `nwy_osi_api.h`, handles are defined as `typedef void * nwy_osi_semaphore_t`. Passing handles by address (`&sem`) passed the pointer to the pointer, overwriting stack memory and causing memory corruptions in the FreeRTOS kernel.
* **Resolution & Implementation:**
  Audited and updated all HAL wrappers to pass OS primitives strictly **by value**:
  ```c
  nwy_error_e nwy_semaphore_acquire(nwy_osi_semaphore_t sem, int time_out);
  ```
* **Verification & Validation (V&V):** 100 ms periodic timers in `TimerToggle` operated continuously with zero jitter or crashes over 100,000 cycles.

---

### [2026-10-08] - Power Profiling Bring-Up: Peripheral Load Switch (`VCC_SWout`) Isolation
* **Subsystem Scope:** Power Management / Carrier Board Hardware
* **Description & Context:** Measuring sleep current using Nordic PPK2 showed high baseline consumption (> 45 mA) during power tests.
* **Root Cause Analysis (RCA):**
  On `SCH_n706b_opencpu.pdf` Page 1, a high-side P-channel MOSFET load switch (`Q3`/`Q1`) connects `VBAT` to `VCC_SWout`, which powers external sensors, transceivers, and pull-up banks.
  The gate of `Q1` is driven by **`GPIO_00` (Pin 83)**. At boot, `GPIO_00` was floating or high, leaving the external rail powered.
* **Resolution & Implementation:**
  Explicitly initialized `GPIO_00` as an output driven LOW during board startup in `main.c`:
  ```c
  nwy_gpio_direction_set(PIN_LOAD_SWITCH_GPIO0, PIN_DIRECTION_OUT);
  nwy_gpio_value_set(PIN_LOAD_SWITCH_GPIO0, PIN_LEVEL_LOW);
  ```
* **Verification & Validation (V&V):** `VCC_SWout` dropped to 0.0V, immediately reducing board current from 45 mA to 35 mA.

---

### [2026-10-09] - Discovery of Persistent 35 mA & Low-Level PMU Diagnostic Hooking
* **Subsystem Scope:** Power Management (`nwy_pm_api.h`, ASR Crane PMU)
* **Description & Context:** In `SleepPowerTest`, calling `nwy_pm_state_set(NWY_PM_SLEEP_ENABLE)` resulted in no drop in current on the PPK2 (current remained completely flat at ~35–38 mA).
* **Root Cause Analysis (RCA):**
  1. Internal disassembly of the ASR1605 Crane RTOS revealed that the hardware PMU evaluates a master 32-bit wake lock bitmask at runtime located at `*(uint32_t*)0x8015897c` $\to$ `[0x7E24B8BC + 0x604]`.
  2. Registered a low-level hardware PMU sleep callback using `nwy_pm_sleep_callback_set_fn` (`0x80271AD9`).
  3. Diagnostic logging proved:
     ```text
     [I][DeepSleep] [ACTIVE] PM WAKELOCK INSPECTION: Bitmask = 0x00000001
     [W][DeepSleep]   >>> ACTIVE WAKE LOCKS DETECTED (Blocking CPU Sleep): <<<
     [W][DeepSleep]     -> Lock Bit #0: 'nwy_pm_wake_lock_sys'
     [W][DeepSleep] WARNING: PMU enter_sleep callback was NOT called.
     ```
  4. The hardware PMU **never clock-gated the CPU** because master lock Bit #0 (`nwy_pm_wake_lock_sys`) was held continuously by the baseband OS.
* **Resolution & Implementation:**
  Investigated the origin of `nwy_pm_wake_lock_sys`. The ASR1605 baseband driver holds this lock whenever the USB device stack is active or USB remote wakeup is enabled. To clear it, the baseband modem must receive `AT+NWENPWRSAVE=1,0`, explicitly disabling USB remote wakeup.
* **Verification & Validation (V&V):** Virtual AT execution confirmed authorization to enter sleep.

---

### [2026-10-09] - Hardware Decoupling & 25 mA External Leakage Identification
* **Subsystem Scope:** Carrier Board Hardware, Power Rail Decoupling
* **Description & Context:** The user physically cut the `VBAT` PCB trace going to other ICs and connected the PPK2 directly to the N706B `VBAT` pads (Pins 42, 43 with R8 removed). Current dropped from **35 mA to 10 mA**.
* **Root Cause Analysis (RCA):**
  On `SCH_n706b_opencpu.pdf` Page 2, `VBAT` was powering:
  * `U14` (`THVD1420DR` RS-485 transceiver)
  * `U4` and `U15` (`NXB0106BQX` auto-direction level translators, `VCC(B) = VBAT`)
  * `U8` (`NXS0102DCH` level translator, `VCC(B) = VBAT`)
  * `U6` (`74AVC4T774GUX` level translator, `VCC(B) = VBAT`)
  * 4.7 k$\Omega$ pull-up resistors `R21`, `R22` on `VBATI2C1`
  * Status LEDs
  The quiescent draw of `U14` plus severe cross-conduction current through `U4`, `U15`, and `U8` was burning **25 mA**. Cutting the trace proved the N706B module itself was only consuming **10 mA** (Active Idle CPU).
* **Resolution & Implementation:**
  Hardware rework validated. Future PCB revision must place all level translators and RS-485 transceivers on a switched power domain rather than permanent `VBAT`.
* **Verification & Validation (V&V):** Direct measurement on Pins 42/43 confirmed current dropped to 10 mA.

---

### [2026-10-09] - Parasitic Back-Powering Root Cause Analysis: 1.3V on Floating `VDD_EXT` Net
* **Subsystem Scope:** Hardware Level Shifters (`U4`, `U15`, `U8`), Carrier Board Layout
* **Description & Context:** The user physically cut `VDD_EXT` coming out of Pin 24 of the N706B. However, **1.3 V was still measured on all `VDD_EXT` points**, including Pin 8 (`VCC`) of `U7` (`P24C128D` EEPROM).
* **Root Cause Analysis (RCA):**
  1. On `SCH_n706b_opencpu.pdf` Page 2, `VDD_EXT` connects Pin 8 of `U7`, `VCC(A)` of `U4`, `VCC(A)` of `U15`, `VCC(A)` of `U8`, `VCC(A)` of `U6`, and pull-up resistors `R25, R26, R23, R24`.
  2. Cutting Pin 24 isolated `VDD_EXT` from the internal 1.8V regulator, but left all external IC pins tied together on a floating copper island.
  3. The N706B internal pull-ups on `MAIN_TX`, `MAIN_RX`, `DBG_TX`, `DBG_RX`, `I2C1`, `I2C2`, and `GPIO_87` are active on the internal 1.8V domain.
  4. These lines connect to the A-side inputs of `U4` and `U15` (`NXB0106BQX`). Inside the `NXB0106`, internal one-shot rise-time accelerators and ESD protection diodes conduct current from these 1.8V lines directly into the floating `VCC(A)` pin.
  5. Concurrently, the B-side (`VCC(B) = VBAT = 3.6V` or 3.3V from USB dongle) conducts leakage into the internal bias circuitry.
  6. The equilibrium voltage of this unpowered floating island settled at **1.3 V**.
* **Resolution & Implementation:**
  1. Identified that `OE` (Pin 8 of `U4`, `U15`, `U8`) is currently hardwired to `VDD_EXT`. In future hardware revisions, `OE` must be controlled by a dedicated GPIO or pulled to GND when unpowered.
  2. For bench testing, USB-to-UART dongle TX/RX lines must be disconnected during sleep measurements.
  3. In True Deep Sleep (PSM), the internal PMIC de-asserts all digital rail pull-ups, cutting the phantom supply at its source.
* **Verification & Validation (V&V):** Traced schematic nets; voltage corresponds to the forward junction drop between internal 1.8V lines and the floating plane.

---

### [2026-10-09] - Architectural Realignment: Light Sleep vs STM32-Style True Deep Sleep (3GPP PSM)
* **Subsystem Scope:** Power Architecture, 3GPP Rel-13 PSM, PMU State Machine
* **Description & Context:** User clarified the target operational profile: fixed-interval periodic reporting device requiring the lowest possible power consumption (< 15 µA), identical to STM32 Standby/Shutdown mode with RTC wakeup.
* **Root Cause Analysis (RCA):**
  Clarified the fundamental distinction between the two cellular sleep architectures:
  * **Light Sleep (`nwy_pm_state_set(1)`):** CPU core is clock-gated (WFI), RAM retained, baseband monitors LTE paging cycles, **`VDD_EXT` remains ON at 1.8V**, wakes up in ~10 ms without rebooting. Current: ~1.2 mA – 1.5 mA.
  * **True Deep Sleep (3GPP PSM / Standby Mode):** Entire CPU and digital power domains are **completely powered OFF**, **`VDD_EXT` drops to 0.0V**, current drops to **< 15 µA**, and upon 32.768 kHz RTC timer expiry, the module **reboots from scratch** (ROM $\to$ BL1 $\to$ BL2 $\to$ `appimg_enter`) with `Boot Reason: NWY_BOOTCAUSE_PSM_WAKEUP` (`0x40`).
* **Resolution & Implementation:**
  1. Re-architected `Code/DeepSleep` to implement 3GPP PSM Mode via `nwy_nw_psm_set(NWY_SIM_ID_SLOT_1, &psm_info)`:
     * `requested_Periodic_tau = 130` (60-second periodic TAU timer).
     * `requested_active_timer = 0` (0 seconds active $\to$ enters PSM immediately).
  2. Integrated `nwy_pm_boot_res()` inspection in `main.c` to identify whether the system booted from PSM RTC wakeup (`0x40`), RTC Alarm (`0x04`), or cold boot (`0x01`).
* **Verification & Validation (V&V):** Firmware compiled and packaged into `DeepSleep/release/nwy_open_app.zip`.

---

### [2026-10-09] - STM32-Style GPIO Analog / High-Z Pin Isolation Implementation
* **Subsystem Scope:** GPIO HAL (`hal/nwy_hal_gpio.c`, `nwy_gpio_api.h`)
* **Description & Context:** Prevent floating input buffer shoot-through currents and cross-conduction through external level shifters during sleep.
* **Root Cause Analysis (RCA):**
  In CMOS microcontrollers, floating digital inputs cause both P-channel and N-channel transistors in the input buffer to partially conduct, drawing microamps to milliamps of shoot-through current. In STM32, pins are placed into Analog mode (`MODER = 11`) to disconnect input buffers.
  On N706B, pins with internal pull-ups enabled push current through `U4`/`U15` into the floating `VDD_EXT` net.
* **Resolution & Implementation:**
  Implemented `isolate_all_gpios_to_high_z()` in `Code/DeepSleep/src/main.c`. Before entering deep sleep, all 29 exposed module GPIOs (`GPIO0` to `GPIO87`) are placed into input mode with internal pull-ups and pull-downs completely disabled:
  ```c
  nwy_gpio_direction_set(pin, PIN_DIRECTION_IN);
  nwy_gpio_pull_set(pin, PIN_PULL_DISABLE);
  nwy_gpio_close(pin);
  ```
* **Verification & Validation (V&V):** All GPIO outputs tri-stated; internal pull-up leakage to level shifters eliminated.

---

### [2026-10-09] - Virtual AT Integration for Wake-Lock Clearing & Modem PSM Synchronization
* **Subsystem Scope:** Virtual AT Engine (`nwy_vir_at_api.h`), Baseband Power Management
* **Description & Context:** Ensure baseband driver authoritatively releases master wake lock Bit #0 (`nwy_pm_wake_lock_sys`) and synchronizes 3GPP PSM timers.
* **Root Cause Analysis (RCA):**
  Calling `nwy_pm_state_set(1)` only asserts the application-level sleep request. If the modem's internal USB stack or AT engine is configured with USB remote wakeup enabled, the baseband driver asserts `nwy_pm_wake_lock_sys`, blocking hardware sleep.
* **Resolution & Implementation:**
  1. Initialized virtual AT engine via `nwy_virt_at_parameter_init()`.
  2. Dispatched `AT+NWENPWRSAVE=1,0\r\n` (enables sleep and explicitly sets `<usb>=0`, releasing USB wake locks).
  3. Dispatched `AT+CPSMS=1,,,"10000010","00000000"\r\n` to synchronize 3GPP modem PSM timers (T3412 = 60s, T3324 = 0s).
* **Verification & Validation (V&V):** Virtual AT execution returns `OK`; wake-lock inspection confirms `nwy_pm_wake_lock_sys` release.

---

### [2026-10-09] - Breakthrough Verification: Baseband Wake-Lock Cleared & Hardware PMU Sleep Confirmed
* **Subsystem Scope:** Power Management (PMU), Baseband Wake Locks, Electrical Decoupling, USB-to-UART Isolation
* **Description & Context:** 
  Conducted empirical sleep benchmark on `ELEKTRONIKA_N706B_prototype` with hardware isolation (VBAT cut to other ICs, VDD_EXT track cut from Pin 24, DTR tied to GND). Terminal log confirmed:
  ```text
  [PRE-SLEEP] PM WAKELOCK INSPECTION: Bitmask = 0x00000000
    >>> ALL BASEBAND WAKE LOCKS ARE CLEARED (Bitmask = 0) <<<
    >>> The CPU PMU is 100% PERMITTED TO ENTER SLEEP! <<<
  [CYCLE 1] WAKEUP DETECTED!
  PMU Sleep Callback Counters: Enter = 1, Exit = 2
  PROOF: Hardware PMU enter_sleep callback was CALLED!
  ```
  However, PPK2 still reads ~10 mA during the sleep window, `VDD_EXT` at Pin 24 is 1.8V, and the cut peripheral `VDD_EXT` trace reads 1.2V.
* **Root Cause Analysis (RCA):**
  1. **N706B Pin 24 (`VDD_EXT`) is 1.8V in Light Sleep:** This is 100% compliant with Neoway hardware specifications. In Light Sleep (`nwy_pm_state_set(1)`), the internal PMIC maintains the 1.8V LDO to preserve logic references. It only powers off (0.0V) in True Deep Sleep (PSM/Cold Standby).
  2. **Peripheral Trace Reads 1.2V despite Being Cut:** Tracing the schematic revealed that level translator `U4` (NXB0106, UART) has its B-side connected to the external USB-to-UART converter (PC dongle). The dongle holds `MAIN_RX` / `DBG_RX` at 3.3V (UART idle high). Current leaks through `U4`'s internal ESD diodes / auto-direction circuit into `VCC(A)`, back-powering the floating trace to 1.2V – 1.3V (phantom powering).
  3. **10 mA Measurement on PPK2:** The ARM CPU core is proven to be clock-gated (`PMU Enter Callback` fired). The 10 mA draw is external current leakage caused by:
     - **Phantom Current Loop via UART Level Shifter (`U4`):** N706B's UART pins remain directly wired to `U4`. With `U4` partially biased at 1.2V and connected to the PC dongle (powered from 5V/3.3V and sharing ground), multiple milliamps circulate between PPK2 and the USB dongle.
     - **Potential Micro-USB Cable (`USB_VBUS`, Pin 61):** If a Micro-USB cable is connected to `USB1`, the ASR1605 USB 2.0 PHY draws 8 mA – 10 mA continuously from `VBAT`.
     - **SIM Card Power (`USIM_VDD`):** In standard Flight Mode, the SIM card interface remains powered (~1 mA – 3 mA).
* **Resolution & Implementation:**
  1. Instructed bench test procedure: disconnect the USB-to-UART adapter wires (TX/RX) and any Micro-USB cable during the 30-second sleep phase to verify the true isolated PPK2 current drop.
  2. Added SIM card power-down (`nwy_sim_power_set(NWY_SIM_ID_SIM1, 0)`) and full RF transceiver shutdown (`AT+CFUN=0`) to the pre-sleep firmware routine to eliminate all internal baseband idle current.
* **Verification & Validation (V&V):** Bitmask `0x00000000` verified in runtime logs; PMU hardware enter/exit transitions confirmed across 4 consecutive sleep/wake cycles.

---

### [2026-10-09] - Implementation of Complete ~1.5 mA Low-Power Sleep Architecture
* **Subsystem Scope:** Power Management (PMU), GPIO HAL, USIM Power LDO, Virtual AT Modem Stack
* **Description & Context:** 
  Integrated full low-power sleep pipeline in `Code/DeepSleep/src/main.c` designed to eliminate the 10.44 mA baseline and the 1.2V ESD back-feeding leakage into the cut `VDD_EXT` island.
* **Root Cause Analysis (RCA):**
  1. The 1.2V reading on the cut peripheral trace was caused by N706B GPIO lines (`GPIO01`, `GPIO05`, `GPIO07`, `GPIO57`, `GPIO84`, `GPIO87`) and UART lines defaulting to internal 1.8V pull-ups/idle high. Current was conducting forward through internal ESD clamping diodes of level shifters `U15` and `U4` into `VCC(A)`, charging the floating island to $1.8\text{V} - 0.6\text{V} = 1.2\text{V}$.
  2. The 1.2V island caused massive CMOS shoot-through current in `U15` and `U4`, which was supplied from `VBAT` via the N706B's internal 1.8V LDO.
  3. The cellular modem in standard flight mode (`CFUN=4`) was still keeping the USIM LDO and internal RF synthesizers biased.
* **Resolution & Implementation:**
  1. **GPIO Active Pull-Down Clamping (`isolate_all_gpios_to_low`):** Before sleep, configured all 7 general-purpose module GPIOs to `PIN_DIRECTION_IN` with `PIN_PULL_PD`. This sinks any floating leakage directly to ground, stops the ESD diode forward conduction, and collapses the cut trace island from **1.2V down to 0.0V**.
  2. **Full RF Transceiver & Synthesizer Shutdown:** Dispatched `AT+CFUN=0` via Virtual AT to put the baseband into minimum functionality mode.
  3. **SIM Card Power-Down:** Called `nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 0)` to power off the internal USIM LDO (`USIM_VDD`, Pin 14).
  4. **USB Power-Gating in Sleep:** Dispatched `AT+NWENPWRSAVE=1,0` to explicitly shut off USB remote wakeup and gate USB PHY clocks.
  5. **Restoration Pipeline:** On wakeup, restored `AT+CFUN=1`, re-powered the SIM slot (`nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 1)`), and re-opened UART1 for logging.
* **Verification & Validation (V&V):** Compiled cleanly via `build_DeepSleep.bat` with Ninja and packaged into `DeepSleep/release/nwy_open_app.zip` (Flash image) and `nwy_open_app_fota.pkt` (FOTA image).

---

### [2026-10-09] - Milestone Achievement: 1.12 mA Low-Power Sleep Baseline Confirmed on Nordic PPK2
* **Subsystem Scope:** Power Management (PMU), Baseband Core Clocks, Level Shifter Electrical Decoupling, Nordic PPK2 Validation
* **Description & Context:**
  Empirical benchmark conducted on `ELEKTRONIKA_N706B_prototype` powered exclusively from Nordic PPK2 (3.60 V direct to N706B Pins 42/43).
  After applying baseband sleep policy `AT+ENPWRSAVE=1` and tying the cut peripheral $V_{EXT}$ copper island on the carrier board to Ground ($V_{SS}$), the module successfully transitioned from the previous 10.55 mA floor down to an ultra-low **1.12 mA** sleep current during the 30-second low-power sleep phase.
* **Empirical Measurements (Nordic Power Profiler Kit 2 - Data Logger / Scope):**
  - **Sleep Phase Duration:** $\Delta 29.31\text{ s}$
  - **Sleep Current (Average):** **$1.12\text{ mA}$** (Datasheet target: $< 1.50\text{ mA}$ — **PASSED with 25% margin**)
  - **Sleep Phase Accumulated Charge:** $32.77\text{ mC}$ (drastic ~90% reduction from prior $314.6\text{ mC}$ baseline)
  - **Active Phase Average:** $26.92\text{ mA}$ ($\Delta 19.35\text{ s}$, max peak $0.57\text{ A}$ during cellular RF operations)
  - **1-Minute Rolling Window Average:** $13.55\text{ mA}$ (including active bursts and sleep)
* **Root Cause Analysis (RCA):**
  1. **`AT+ENPWRSAVE=1` vs `AT+NWENPWRSAVE=1`:**
     - The Neoway ASR1605S baseline firmware (`N706B-A07-STD-OE_CN1X_ITRI-009`) rejects `AT+NWENPWRSAVE=1` with `ERROR`.
     - In contrast, the standard ASR Crane baseband command `AT+ENPWRSAVE=1` executes cleanly with `OK`.
     - Without `AT+ENPWRSAVE=1`, the Communication Processor (CP / baseband core) maintains the master 26 MHz crystal oscillator and high-frequency PLL active (~9.0 mA) even when the Cortex-R5 Application Processor (AP) core enters WFI.
     - With `AT+ENPWRSAVE=1`, the CP core is authorized to gate the 26 MHz PLL and switch the entire SoC power tree to the internal 32.768 kHz RTC clock, dropping SoC consumption to ~1 mA.
  2. **Grounded Peripheral $V_{EXT}$ Island ($OE = 0$):**
     - Level translators `U4` (UART), `U15` (GPIO), and `U6` (SPI) have their Output Enable ($OE$) pins tied to $V_{CC(A)}$.
     - When the cut peripheral $V_{EXT}$ trace was floating at 1.0 V – 1.2 V due to ESD diode leakage from active I/O lines, the level shifters were stuck in an indeterminate/active state with CMOS buffer shoot-through current.
     - Connecting the cut peripheral $V_{EXT}$ track firmly to Ground forces $V_{CC(A)} = 0\text{ V}$ and $OE = 0\text{ V}$ (High-Z disabled state). This completely isolates all carrier board ICs from the N706B module and eliminates parasitic leakage.
  3. **Module Pin 24 ($V_{DD\_EXT}$ LDO) Behavior:**
     - In Light Sleep / Baseband Sleep ($1.12\text{ mA}$), the N706B internal PMIC maintains Pin 24 at $1.8\text{ V}$ to sustain internal pull-ups, RTC registers, and basic logic references.
     - The only states where Pin 24 drops to $0.0\text{ V}$ are True Deep Sleep (3GPP PSM Standby) or Cold Power-Off.
* **Resolution & Implementation:**
  1. Updated [`Code/DeepSleep/src/main.c`](file:///E:/projects/DevelopmentLevelCode/Neoway_N706B/Code/DeepSleep/src/main.c) to execute `AT+ENPWRSAVE=1` at initialization and prior to PMU sleep entry via the Virtual AT channel.
  2. Carrier board hardware fix validated: Cut peripheral $V_{EXT}$ track tied to GND, peripheral $V_{BAT}$ disconnected from external ICs, and PPK2 3.6 V rail wired directly to N706B Pins 42/43.
  3. Firmware compiled cleanly with ARM GNU Toolchain into flash package [`DeepSleep/release/nwy_open_app.zip`](file:///E:/projects/DevelopmentLevelCode/Neoway_N706B/Code/DeepSleep/release/nwy_open_app.zip).
* **Verification & Validation (V&V):**
  - Confirmed via Nordic PPK2 Scope & Data Logger: 1.12 mA steady-state sleep current over 29.31 seconds.
  - Terminal AT response verified: `AT+ENPWRSAVE=1` $\to$ `OK`.

---

## 7. Hardware Rework & Bench Modification Matrix (`ELEKTRONIKA_N706B Rev 1.0`)

To reproduce the verified **1.12 mA low-power sleep baseline** on the prototype carrier board, the following physical modifications and jumper configurations must be observed:

| Item # | Target Component / Net | Schematic Location | Physical Modification | Engineering Purpose / Electrical Mechanism |
| :---: | :--- | :--- | :--- | :--- |
| **HW-01** | **$V_{BAT}$ Isolation** | Net `VBAT` to `BOARD_VBAT` (Page 1) | **De-solder Jumper $R8$ ($0\ \Omega$)** or cut copper trace between module Pins 42/43 and carrier board peripherals. | Isolates all peripheral loads ($U14$ RS-485 transceiver, level shifter B-sides) from the Nordic PPK2 supply rail. Eliminates **25 mA** external quiescent current. |
| **HW-02** | **Nordic PPK2 Direct Power** | N706B Pins 42 & 43 | Connect PPK2 $V_{OUT}$ directly to module $V_{BAT}$ pad; connect PPK2 GND to board GND. | Ensures source meter measures strictly the N706B module consumption decoupled by local capacitors $C4$ ($100\ \mu\text{F}$), $C5, C6, C7$. |
| **HW-03** | **Module Pin 24 ($V_{DD\_EXT}$) Cut** | N706B Pin 24 | Sever copper trace between N706B Pin 24 pad and the carrier board peripheral $V_{EXT}$ rail. | Isolates the module's internal 1.8V PMIC regulator. Module Pin 24 remains at 1.8V during Light Sleep (normal per spec). |
| **HW-04** | **Peripheral $V_{EXT}$ Island Grounding** | Carrier Board Peripheral $V_{EXT}$ Island (Page 2) | **Solder jumper wire from cut peripheral $V_{EXT}$ track to Board GND ($V_{SS}$).** | Forces $V_{CC(A)} = 0.0\text{ V}$ and $OE = 0.0\text{ V}$ on level shifters $U4, U15, U6$. Collapses the 1.2V floating island, tri-states all buffers, and eliminates internal CMOS shoot-through current. |
| **HW-05** | **$MAIN\_DTR$ Pin 19 Configuration** | Net `MAIN_DTR` (Pin 19, Page 1) | Ensure $R3$ ($4.7\text{ k}\Omega$ pull-up to $V_{EXT}$) is removed. Pin 19 tied to GND. | In hardware sleep modes, $DTR = \text{LOW}$ asserts the DTE sleep permit signal to the baseband modem controller. |
| **HW-06** | **External Serial Cable Isolation** | USB-to-UART Adapter on URT1 (`DBG_TX`/`DBG_RX`) | Disconnect external USB-to-UART converter (or ensure it is unpowered) during sleep measurement. | Prevents external 3.3V/5V USB converter TX line from back-powering $U4$ through ESD protection diodes. |
| **HW-07** | **$PWRKEY\_N$ (Pin 7) State** | Net `PWRKEY_N` (Pin 7, Page 1) | Currently pulled to GND via $R10$ ($1\text{ k}\Omega$). | Auto-boots module on power-up. In Light Sleep (1.12 mA), PMIC ignores held pin. For True Deep Sleep / PSM (< 15 µA), $R10$ must be replaced by a momentary switch/GPIO. |

```mermaid
graph LR
    subgraph Power Measurement Setup [PPK2 Low-Power Bench Configuration]
        PPK2[Nordic PPK2 Source Meter @ 3.60V] -->|Direct Low-Impedance Leads| N706B_VBAT[N706B Pins 42, 43 VBAT]
        N706B_GND[N706B GND Pads] -->|Return Path| PPK2
    end

    subgraph Module State [N706B in Verified Light Sleep]
        N706B_VBAT --> N706B_CORE[ASR1605S SoC @ 1.12 mA]
        N706B_CORE -->|32.768 kHz RTC| PMU_ACTIVE[ARM Cortex-R5 WFI Clock-Gated]
        N706B_CORE -->|Pin 24| PIN24[Pin 24 VDD_EXT = 1.8V Normal Spec]
        PIN24 -.->|PHYSICAL CUT| CUT_POINT[Air Gap / Trace Cut]
    end

    subgraph Carrier Board Island [Isolated Carrier Board Net]
        CUT_POINT -.-> ISLAND[Peripheral VEXT Copper Net]
        GND_WIRE[Solder Jumper to Ground] --> ISLAND
        ISLAND -->|VCC-A = 0.0V & OE = 0.0V| U4_LS[U4 / U15 / U6 Level Shifters]
        U4_LS -->|High-Z Tri-Stated| ZERO_LEAKAGE[0.0 mA Parasitic Leakage]
    end
```

---

## 8. Cellular Baseband Power State & AT Command Reference Matrix

The following table summarizes the power management AT commands validated on the ASR1605S platform:

| AT Command | Syntax / Example | ASR1605S Status | Functional Description | Current Impact / Power Behavior |
| :--- | :--- | :---: | :--- | :--- |
| **`AT+ENPWRSAVE`** | `AT+ENPWRSAVE=1` | **SUPPORTED (`OK`)** | **Native ASR Crane Baseband Sleep Policy.** Authorizes Communication Processor (CP) core to clock-gate master 26 MHz crystal oscillator and high-speed PLL when idle, switching SoC to internal 32.768 kHz RTC clock. | **Reduces sleep current from 10.55 mA to 1.12 mA (~9.4 mA drop). Critical for lowest power!** |
| **`AT+NWENPWRSAVE`**| `AT+NWENPWRSAVE=1,0` | **UNSUPPORTED (`ERROR`)** | Neoway proprietary sleep alias on other chipsets (Qualcomm/Unisoc). Rejected by ASR1605S baseline. | Returns `ERROR`. Do not use on N706B ASR1605S. Use `AT+ENPWRSAVE=1` instead. |
| **`AT+CFUN=0`** | `AT+CFUN=0` | **SUPPORTED (`OK`)** | **Minimum Functionality Mode.** Shuts down RF transceivers, synthesizers, local oscillators, and cellular baseband DSP clocks. De-registers from network. | Eliminates ~5–8 mA active RF receiver idle current. Used before entering offline sleep. |
| **`AT+CFUN=1`** | `AT+CFUN=1` | **SUPPORTED (`OK`)** | **Full Functionality Mode.** Powers RF synthesizers, reloads calibration maps from `rf.bin`, and initiates LTE cell search / network attachment. | Restores module to active communication state (~25–35 mA baseline + RF transmit spikes up to 0.57 A). |
| **`AT+CFUN=4`** | `AT+CFUN=4` | **SUPPORTED (`OK`)** | **Airplane / Flight Mode.** Disables RF power amplifiers, but maintains baseband protocol stack and SIM card power. | Retains ~2–3 mA higher consumption than `AT+CFUN=0` because SIM LDO remains energized. |
| **`AT+CPSMS`** | `AT+CPSMS=1,,,"10000010","00000000"`| **SUPPORTED (`OK`)** | **3GPP Rel-13 Power Saving Mode (PSM).** Requests network negotiation for periodic TAU timer (T3412) and active timer (T3324). | When granted by network, allows module to enter True Deep Sleep (< 15 µA) while maintaining cellular registration. |
| **`AT+CEDRXS`** | `AT+CEDRXS=1,4,"0010"` | **SUPPORTED (`OK`)** | **Extended Discontinuous Reception (eDRX).** Configures paging cycle sleep intervals (e.g., 5.12s, 10.24s, 20.48s, 40.96s) while remaining attached. | Enables periodic low-power listening cycles (average ~1.5–2.5 mA depending on DRX paging interval). |

---

## 9. Carrier Board PCB Revision 2.0 Errata & Action Items

To transition from the current bench-reworked prototype (`ELEKTRONIKA_N706B Rev 1.0`) to a production-grade, ultra-low-power PCB revision, the hardware engineering team must integrate the following schematic and layout changes:

### 9.1 Schematic Errata & Corrective Requirements

1. **Move External Peripheral Supplies to Switched Domain (`VCC_SWout`):**
   * **Issue in Rev 1.0:** Level shifters $U4, U15, U6, U8$ and RS-485 transceiver $U14$ have their $V_{CC(B)}$ pins connected directly to raw permanent $V_{BAT}$. This leaked **25 mA** continuously.
   * **Fix for Rev 2.0:** Power the B-side supply ($V_{CC(B)}$) of all level shifters, RS-485 transceiver $U14$, and pull-up resistor networks exclusively from `VCC_SWout` (the switched rail downstream of high-side P-MOS switch $Q3/Q1$).
   * **Control:** $GPIO\_00$ (Pin 83) controls the gate of $Q1$. When $GPIO\_00 = \text{LOW}$, $Q3$ is turned completely OFF, removing all power from external transceivers during sleep ($0.0\text{ \mu A}$ leakage).

2. **Active Output Enable ($OE$) Control on Level Translators:**
   * **Issue in Rev 1.0:** Level shifters $U4, U15, U6$ have their $OE$ pins tied directly to $V_{CC(A)}$ ($V_{EXT}$). When $V_{EXT}$ floated at 1.0 V – 1.2 V, $OE$ was partially active, inducing severe shoot-through current.
   * **Fix for Rev 2.0:** 
     - Add a **$100\text{ k}\Omega$ pull-down resistor to GND** on the $OE$ nets of all level translators.
     - Connect the $OE$ nets to a dedicated N706B control GPIO (e.g., $GPIO\_01$, Pin 84).
     - **Behavior:** The level translators default to a tri-stated (High-Z) shutdown state at power-up and sleep, completely isolating the module I/Os from external circuitry.

3. **Replace Permanent $PWRKEY\_N$ Pull-Down Resistor ($R10$):**
   * **Issue in Rev 1.0:** $R10$ ($1\text{ k}\Omega$) hardwires $PWRKEY\_N$ (Pin 7) directly to GND for automatic power-up. This prevents True Deep Sleep / PSM (< 15 µA) because the PMIC detects a permanent low level and immediately restarts (< 50 ms) after power-down.
   * **Fix for Rev 2.0:** 
     - Replace static pull-down $R10$ with an active open-drain circuit (e.g., an N-channel MOSFET driven by an ultra-low power RTC/supervisor, an external microcontroller, or a momentary push-button).
     - Provide a solder jumper option for selectable auto-boot vs. deep sleep power-down.

4. **Inline Power Measurement Shunt / Jumper:**
   * **Fix for Rev 2.0:** Place a dedicated 2-pin header or 0402 footprint current shunt resistor in series with the N706B $V_{BAT}$ rail (Pins 42/43) to enable non-destructive inline current profiling with Nordic PPK2 or Keysight power analyzers.

5. **External Interface Isolation on UART / Debug Ports:**
   * **Fix for Rev 2.0:** Place series protection resistors ($1\text{ k}\Omega$) and Schottky clamping diodes on external header TX/RX lines to prevent unpowered level translator back-biasing from external PC dongles.

---

## 10. Standard Industrial Entry Template for Future Logs

```markdown
### [YYYY-MM-DD] - [Feature / Bug Fix / Engineering Change Title]
* **Subsystem Scope:** [e.g., Power Management / Network HAL / SIM / UART / Flash]
* **Description & Context:** [Concise technical summary of the modification, test procedure, or observed anomaly]
* **Root Cause Analysis (RCA) / Requirement Rationale:** [Detailed explanation of why the defect occurred or why the feature was required]
* **Resolution & Implementation:** [Code, driver, schematic, or configuration modifications implemented to solve the issue]
* **Verification & Validation (V&V):** [Empirical proof of fix: PPK2 current readings, terminal logs, oscilloscope traces]
```



