========================================================================
   NEOWAY N706B OPENCPU - SIM CARD API DIAGNOSTIC TEST
========================================================================

This project implements a dedicated automated diagnostic test suite over the Debug UART (UART4)
to test and verify the Neoway N706B OpenCPU SIM Card HAL APIs.

1. DIRECTORY STRUCTURE
-----------------------
  SimCardTest/
    build_SimCardTest.bat - Build execution script
    README.txt            - This documentation file
    inc/
      hal/
        os/nwy_hal_os.h    - Corrected OS Wrapper APIs (Mutexes, Semaphores, Timers)
        sim/nwy_hal_sim.h  - SIM Card API Wrappers
        uart/nwy_hal_uart.h- UART Wrapper APIs
    src/
      main.c              - Core monitor task, self-test scanning logic, and URC callback handlers
      hal/
        os/nwy_hal_os.c    - OS Wrapper Implementation
        sim/nwy_hal_sim.c  - SIM Wrapper Implementation
        uart/nwy_hal_uart.c- UART Wrapper Implementation

2. HOW TO BUILD
---------------
1. Double-click or run `.\build_SimCardTest.bat` from a terminal in this directory.
2. The compilation will use Ninja/CMake to generate the binary.
3. The packaged firmware will be generated at:
   `SimCardTest\release\nwy_open_app.zip`

3. HOW TO FLASH AND RUN
------------------------
1. Open the Aboot flashing tool (`aboot.exe`).
2. Load the zip package located at `SimCardTest\release\nwy_open_app.zip`.
3. Put the board into download mode (send AT$MYDOWNLOAD=1 or bridge BOOT pin to GND if needed).
4. Run download in SWDownloader/Aboot.
5. Once flashed, connect your serial monitor to the Debug UART COM port (COM20) at 115200 baud.
6. On startup, the diagnostic self-test suite executes automatically and prints a formatted report.

4. AUTOMATED DIAGNOSTIC TESTS EXECUTED
--------------------------------------
The system executes the following non-interactive diagnostics on startup:
* **Active SIM Slot Scan:** Retrieves currently active configured slot (Slot 1 or Slot 2).
* **SIM Insertion & Status Scan:** Queries raw SIM presence and status codes on both slots.
* **Credentials Query:** If a card is detected, queries raw IMSI, ICCID, and MSISDN.
* **PIN Lock Query:** Queries current PIN lock protection status and retry attempts remaining.
* **Hotplug URC Callback Test:** Confirms URC callback handlers are correctly registered to process SIM hotplug and status change events.
* **Heartbeat Periodicity:** Prints a heartbeat log to UART every 10 seconds to indicate healthy background operations.

5. DOMAIN RULES & CAVEATS
--------------------------
* SIM index mapping: The SDK uses 0-based indexing (0 = Slot 1, 1 = Slot 2). The HAL wrappers translate user-facing slot IDs (1 or 2) into the correct SDK enums, preventing slot mismatches.
* Safe context execution: All SIM HAL functions are safely executed within the background monitor task context, ensuring the UART thread remains responsive.
* SIM Detection API Limit: Attempting to call `nwy_sim_detect_set` returns `NWY_GEN_E_PLAT_NOT_SUPPORT` (-10) on ASR1605S. The modem core handles presence detection pins automatically, so manual configuration of trigger modes is disabled by the SDK.
* URC Register empty constraint: Registering the URC callback on an inactive or empty SIM slot (like Slot 2 when empty) returns `NWY_FAIL` (0).
* Hotplug Hardware requirement: Dynamic insertion/removal notifications require the PCB layout to connect the physical SIM socket switch pin (SIM_DET) to the module. If left floating on the EVB, status will only update on device reboot.
========================================================================
