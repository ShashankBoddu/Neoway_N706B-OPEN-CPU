# Neoway N706B OpenCPU Development Guide

This document tracks the environment setup, architectural constraints, issues encountered, and the professional engineering workflow established for Neoway N706B OpenCPU firmware development.

---

## 1. Environment Setup Overview
- **Chipset:** ASR1605S (ARM Cortex-R5 @ 614 MHz)
- **Toolchain:** Bundled GNU Arm Embedded Toolchain 10.3.1 (`arm-none-eabi-gcc`), CMake, and Ninja (located in SDK `tools/` and `prebuilts/`).
- **Core SDK Location:** `E:\projects\DevelopmentLevelCode\Neoway_N706B\N706B-A07-STD-OE_CN1X_ITRI-009_SDK\N706B-A07-STD-OE_CN1X_ITRI-009_SDK`
- **Working Code Directory:** `E:\projects\DevelopmentLevelCode\Neoway_N706B\Code`

---

## 2. Critical Issues & Solutions

### A. Spaces and Path Length
- **Issue:** The original path contained spaces (`"Devolapment files"`) and exceeded 240 characters. This caused CMake and Ninja to fail with fatal expansion and syntax errors (`"cannot open file"`, `"missing variable"`).
- **Solution:** Relocated the entire project to `E:\projects\DevelopmentLevelCode\Neoway_N706B`.
- **Rule:** Keep the SDK and code in a path with **no spaces** and minimal depth.

### B. Rigid SDK Structure
- **Issue:** The original SDK CMakeLists.txt was hardcoded to build only internal demos in `nwy_test_demo/`.
- **Solution:** Refactored the root `CMakeLists.txt` in the SDK to introduce the `CUST_PROJ_DIR` variable, enabling the build system to compile any external project folder dynamically:
  ```cmake
  if(CUST_PROJ_DIR)
      include_directories(${CUST_PROJ_DIR}/inc)
      file(GLOB_RECURSE CUST_SOURCES ${CUST_PROJ_DIR}/src/*.c)
  endif()
  ```

### C. Build Failures Due to Stale Cache
- **Issue:** After moving the SDK, the `out/` folder contained stale absolute paths, causing Ninja build failures.
- **Solution:** The automated batch build scripts delete the `out/` folder before invoking CMake.

### D. GPIO Pin Mappings and Modem Core Conflicts (Watchdog Resets)
- **Issue:** Physical schematic net names (e.g., net `GPIO1` on Pin 20 `MAIN_RI`) do not match software GPIO IDs. Writing to modem-controlled pins like `MAIN_RI` (`GPIO[4]`) triggers register contentions with the Cellular Processor (CP), causing watchdog panic reboots.
- **Solution:** Cross-reference physical pins with `N706B-CN (ASR1605S) GPIO Configuration Form`:
  * Pin 21 (`GPIO_07` / net `GPIO2`): Software GPIO ID 7 (Safe general-purpose GPIO).
  * Pin 26 (`GPIO_05` / net `I2S_MCLK`): Software GPIO ID 5 (Safe general-purpose GPIO).
  * Pin 20 (`MAIN_RI` / net `GPIO1`): Software GPIO ID 4 (**CP-owned, DO NOT drive as output!**).
  * Pin 25 (`STATUS` / net `STATUS`): Software GPIO ID 70 (Outputs high on boot, controls STATUS LED).
  * Pin 16 (`NET_STATUS` / net `NET_STATUS`): Software GPIO ID 69 (Outputs network status PWM).
  * Pin 83 (`GPIO_00`): Software GPIO ID 0 (Controls peripheral load switch `Q3`/`Q1`).
  * Pin 84 (`GPIO_01`): Software GPIO ID 1 (Safe general-purpose GPIO).
  * Pin 85 (`GPIO_84`): Software GPIO ID 84 (Safe general-purpose GPIO).
  * Pin 86 (`GPIO_57`): Software GPIO ID 57 (Safe general-purpose GPIO).
  * Pin 87 (`GPIO_87`): Software GPIO ID 87 (Safe general-purpose GPIO).

### E. CMake Structure and Project Layout
- **Issue:** Unlike ESP-IDF which builds from within project folders, Neoway SDK compiles from the SDK root. It automatically scans `${CUST_PROJ_DIR}/src/*.c` recursively and includes `${CUST_PROJ_DIR}/inc`.
- **Solution:** Retain the standardized `src/` and `inc/` folder layout.

### F. Neoway SDK SIM Slot Index Mismatch (Off-by-One)
- **Issue:** `nwy_sim_id_e` is 0-indexed (`NWY_SIM_ID_SLOT_1` = 0, `NWY_SIM_ID_SLOT_2` = 1). Passing an index of `1` targets Slot 2, which is unpopulated, leading to `SIM Status: NOT READY`.
- **Solution:** Map Slot 1 to `NWY_SIM_ID_SLOT_1` (0) and Slot 2 to `NWY_SIM_ID_SLOT_2` (1).

### G. DHCP Placeholder IP (0.0.0.0) False Positive
- **Issue:** On boot, `nwy_data_call_info_get()` returns `"0.0.0.0"` before network IP assignment. A simple string length check evaluates this as a valid connection.
- **Solution:** Add `strcmp(call_info.ipv4_str, "0.0.0.0") != 0` validation before confirming connection.

### H. Data Call Request Spamming
- **Issue:** Continually invoking `nwy_data_call_start()` every tick floods the baseband core.
- **Solution:** Use a state flag (`s_data_call_started`) to issue the connection request only once per disconnected state.

### I. URC Thread Context Deadlocks
- **Issue:** Executing blocking modem commands (like `nwy_sms_msg_send()`) directly inside an asynchronous URC callback deadlocks the modem's notification thread, triggering watchdog resets.
- **Solution:** Buffer the payload and release an OSI semaphore (`nwy_osi_semaphore_t`) to wake up a background task that safely performs the operation.

### J. OS Primitive Handles Passed by Value (Typo in API Docs)
- **Issue:** Official documentation claims OS helper functions take pointer references (`nwy_osi_semaphore_t *sem`). In reality, `nwy_osi_api.h` defines them as void pointers (`typedef void * nwy_osi_semaphore_t`). Passing them by address (`&sem`) corrupts memory and causes crashes.
- **Solution:** Always pass timer, semaphore, and mutex handles strictly **by value**.

### K. Mandatory Non-Zero Event Count in Thread Creation
- **Issue:** Passing `0` for `event_count` in `nwy_thread_create` causes the SDK to return `NWY_FAIL`.
- **Solution:** Set `event_count` to a value greater than 0 (typically 10 or 50).

### L. SIM Hotplug Hardware Detection Restrictions
- **Issue:** Calling `nwy_sim_detect_set()` returns `NWY_GEN_E_PLAT_NOT_SUPPORT` (-10) on ASR1605S.
- **Solution:** The ASR1605S baseband manages detection lines automatically. Register the URC callback via `nwy_hal_sim_register_urc_cb()`.

### M. Modem Teardown Ordering & Unpowered SIM Watchdog Crashes
- **Issue:** Disabling SIM LDO power via `nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 0)` *before* issuing virtual AT power commands (`AT+ENPWRSAVE=1`) panics the Communication Processor (CP) parser, causing a hardware watchdog reset (`□`).
- **Solution:** Enforce strict teardown ordering:
  1. `AT+ENPWRSAVE=1` (authorizes baseband clock gating while AT parser and SIM are active).
  2. `AT+CFUN=0` (minimum functionality / RF shutdown).
  3. Wait 1500 ms for complete network detachment.
  4. `nwy_sim_power_set(NWY_SIM_ID_SLOT_1, 0)` (**last** among modem ops).
  5. Clamp `GPIO0=0` (peripheral cutoff) and isolate GPIOs.
  6. Enable PMU sleep via `nwy_pm_state_set(NWY_PM_SLEEP_ENABLE)`.

### N. FreeRTOS Task Lifecycle & Thread Termination Rule
- **Issue:** Returning from a FreeRTOS task function past its closing brace `}` corrupts the Task Control Block (TCB) stack and panics the kernel. Furthermore, long blocking calls (e.g. `MQTTYield(..., 1000)`) prevent clean task joining during sleep teardown.
- **Solution:** Schedulers must slice yield intervals to 100 ms, track a running boolean flag (`g_yield_thread_running`), and explicitly invoke `nwy_thread_exit(NULL)` upon loop exit.

### O. Virtual AT String Matching & Initial Boot Delay Fix
- **Issue:** Setting `info.ok_fmt = "\r\nOK\r\n"` with a 10s timeout in `nwy_vir_at_api.h` causes a 33-second silent stall at boot because ASR1605 Crane OS returns `"OK\r\n"` (without leading CRLF) or initial boot URCs.
- **Solution:** Always set `info.ok_fmt = "OK"` and `info.err_fmt = "ERROR"` (substring matches) with a **2-second timeout**.

### P. Half-Duplex RS-485 Sequential Driver vs Asynchronous Callbacks
- **Issue:** Attaching asynchronous RX ISR callbacks (`nwy_uart_rx_cb_register()`) on UART ports causes kernel memory panics when closing and re-opening the port across sleep/wake cycles.
- **Solution:** Use direct non-blocking polling (`nwy_uart_rx_cb_register(g_rs485_fd, NULL)` and `nwy_uart_read()`) structured in sequential 10-second Transmit and 10-second Receive windows modeled after industrial Modbus engines.

---

## 3. How to Build Your Projects
Each project folder contains its dedicated `build_<ProjectName>.bat` script.

1. Open a terminal in the project directory (e.g., `Code/DeepSleep`).
2. Run the build script:
   ```cmd
   .\build_<ProjectName>.bat
   ```
3. Output artifacts:
   - Raw binary: `SDK_ROOT\out\bin\nwy_open_app.bin`
   - Flash package: `<ProjectFolder>\release\nwy_open_app.zip` (For Aboot)
   - FOTA package: `<ProjectFolder>\release\nwy_open_app_fota.pkt` (For OTA updates)

---

## 4. Flashing the Module
1. **Tool:** Launch `AbootDownload` (`arelease.exe` / `aboot.exe`).
2. **Binary:** Select `nwy_open_app.zip` from your project's `release/` folder.
3. **Download Mode:** In serial terminal on MAIN_UART, send `AT$MYDOWNLOAD=1`.
4. **Flashing:** Click 'Start' in SWDownloader; the module flashes in ~3 seconds.

---

## 5. Low-Power Sleep Configuration & Hardware Rework (568.69 uA Sub-Milliamp Record)
For full engineering details, circuit schematics, multimeter ground truths, and Nordic PPK2 captures across all development stages (from 35 mA down to 568.69 uA), refer to the comprehensive developer log:
- **[DEV_LOG.md](DEV_LOG.md)** (or **[DEV.md](DEV.md)**)

### Quick Hardware Setup Summary:
1. **New Board Validated (Zero Trace Cuts Required):** The latest Stage 4 validation was performed on a **new carrier board with completely intact, uncut copper tracks** (VBAT and VEXT tracks are **NOT cut**!).
2. **P-MOSFET Gate Pull-Up Resistors (R4, R27):** Upgraded from 100k ohm to **26k ohm** to ensure full cutoff (VGS = 0V) during sleep.
3. **Master Power Control (GPIO0, Pin 83):** In active mode, GPIO0 is HIGH (0.909V, saturating NPNs); in sleep mode, GPIO0 is driven LOW (0.00V), completely turning off Q1 and Q10 via the 26k pull-ups.
4. **Baseband Sleep Policy:** Run `AT+ENPWRSAVE=1` (standard ASR Crane OS command; note that `AT+NWENPWRSAVE` returns ERROR on this chipset).
5. **Empirical Bench Results:**
   - Active Current: **44.05 mA** (23.45s)
   - Sleep Current: **568.69 uA** (29.86s flatline)
   - Switched Rails in Sleep: VCC_SWout = 0.00V, RS-485 VCC = 0.32V (100% isolated).

---

## 6. Integrated Production Benchmark: `Network_Mqtt_Rs485_DeepSleep`
Located in `Code/Network_Mqtt_Rs485_DeepSleep/`, this application measures the end-to-end average current consumption profile of standard periodic IoT operation:
- **RS-485 Communication Test**: Powers `THVD1420DR` transceiver (`U14`) via `GPIO0`, sets direction via `GPIO87` (`HIGH`=TX, `LOW`=RX), sends probe packet at 9600 baud over `URT2` (MAIN_UART, Pins 17/18), and listens for 3 seconds.
- **Cellular Attachment & Data Call**: Attaches to LTE, reads signal quality (CSQ) and network operator, and starts a packet data call on CID 1.
- **MQTT Telemetry Stream**: Connects to `mqtt.fervidlabs.in:1883`, publishes structured JSON telemetry to `nwy_n706b/fervid/tx`, and gracefully terminates the session.
- **Low-Power Deep Sleep (30s)**: Shuts down RF (`AT+CFUN=0`), cuts USIM LDO power, enforces baseband 32 kHz clock gating (`AT+ENPWRSAVE=1`), isolates carrier GPIOs, clamps `GPIO0=0` (0V on `VCC_SWout` and 0.32V on RS-485 `VCC`), and enters Cortex-R5 PMU sleep for 30 seconds (targeting **568.69 uA** floor).
- **Periodic Repetition**: Wakes up automatically every 30 seconds to repeat the cycle indefinitely for continuous Nordic PPK2 power profiling.
- **Build Artifacts**: Run `build_Network_Mqtt_Rs485_DeepSleep.bat` to generate `release/nwy_open_app.zip` and `release/nwy_open_app_fota.pkt`.

---

## 7. UART / RS-485 Hardware & Firmware Ground Truth (Mini EVB vs Custom Board)

### 7.1 UART Port Role Mapping in OpenCPU SDK
As documented in the OpenCPU SDK Application Guide:
- `"URT1"` (`NWY_NAME_UART1`): Module Pins 38 & 39 (`DBG_RXD`, `DBG_TXD`). Exclusively used for diagnostic log output and telemetry (`nwy_hal_uart_log_i` / `printf`) at 115200 baud.
- `"URT2"` (`NWY_NAME_UART2`): Module Pins 17 & 18 (`MAIN_TXD`, `MAIN_RXD`). Brought out on EVB header `J606` (labeled `UART1` on EVB silkscreen, Header 11). **Dedicated port for the RS-485 differential transceiver (`THVD1420DR`)**.
- `"URT3"` (`NWY_NAME_UART3`): Module Pins 28 & 29 (`AUX_RXD`, `AUX_TXD`). Brought out on EVB header `J600` (labeled `UART2` on EVB silkscreen, Header 12).
  > [!WARNING]
  > **GNSS Hardware Constraint on `URT3`:** On N706B sub-models with integrated GNSS (such as `N706B-CN-10`), `URT3` is internally hardwired and dedicated by the baseband firmware (`cp.bin`) to the HXXT CC1161W GNSS chip. As stated in the Neoway Hardware User Guide: *"AUX_UART is unavailable in sub-models with GNSS"*. Therefore, RS-485 must always be routed to **`URT2`**.

### 7.2 RS-485 Transceiver Pinout on MAIN UART (`URT2`)
- **Module Pin 18 (`MAIN_TXD`, Output)** $\to$ Transceiver Pin 4 (`DI` / Driver Input).
- **Module Pin 17 (`MAIN_RXD`, Input)** $\leftarrow$ Transceiver Pin 1 (`RO` / Receiver Output).
- **Module Pin 87 (`GPIO87`, Direction)** $\to$ Transceiver Pins 2 & 3 (`/RE` & `DE`) via `Q7` N-MOSFET level shifter.
  - `HIGH` (1.8V): Transmit Mode (`DE=1`, `/RE=1`).
  - `LOW` (0.0V): Receive Mode (`DE=0`, `/RE=0`).
- **Module Pin 83 (`GPIO00`, Power)** $\to$ Master Peripheral Switch (`Q5/Q10` P-MOSFET).
  - `HIGH` (1.8V): Transceiver VCC powered (3.9V).
  - `LOW` (0.0V): Transceiver VCC isolated (0.32V floating, 0.0 uA draw).
- **On Mini EVB Header J606 (labeled `UART1`, Header 11):**
  - **Pin 1:** `VCCIO_UART`
  - **Pin 2:** `MAIN_RXD` (Module Input) $\leftarrow$ Connect Transceiver `RO` (Pin 1) or external TX.
  - **Pin 3:** `MAIN_TXD` (Module Output) $\to$ Connect Transceiver `DI` (Pin 4) or external RX.
  - **Pin 4:** `GND` $\to$ Common ground.
- **Serial Configuration:** 9600 baud, 8 data bits, no parity, 1 stop bit (8-N-1), no flow control.



