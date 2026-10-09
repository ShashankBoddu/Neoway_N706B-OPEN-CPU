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
