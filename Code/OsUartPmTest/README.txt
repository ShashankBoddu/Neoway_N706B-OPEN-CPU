================================================================================
OsUartPmTest - OpenCPU Test Project
================================================================================

This project isolates, tests, and provides helper libraries (HAL) for the 
Operating System, UART, and Power Management features of the Neoway N706B 
OpenCPU SDK.

--------------------------------------------------------------------------------
1. Directory Structure
--------------------------------------------------------------------------------
* inc/hal/
  - os/nwy_hal_os.h: Threading, Sleep, Mutexes, Semaphores, and Timer APIs.
  - uart/nwy_hal_uart.h: Serial communications driver helpers.
  - pm/nwy_hal_pm.h: System power states and Real-Time Clock APIs.
* src/
  - hal/os/nwy_hal_os.c: Thread/Timer/Sync implementations.
  - hal/uart/nwy_hal_uart.c: Serial driver implementation.
  - hal/pm/nwy_hal_pm.c: Voltage queries, boot reasons, and time conversions.
  - main.c: Application boot entry and command handler loop.
* build_OsUartPmTest.bat: Compilation and Aboot packaging automation script.

--------------------------------------------------------------------------------
2. Interactive Commands
--------------------------------------------------------------------------------
Connect to the module via the UART4 (Debug UART) interface (corresponds to "URT1" in SDK code) at 115200 baud rate. 
Send the following characters to execute tests:
* h: Display the help menu.
* r: Reboot the OpenCPU system.
* p: Power down the system.
* v: Query and print battery voltage level (mV).
* t: Query and print calendar RTC date and time.
* s: Perform non-blocking semaphore lock, release, and acquire tests.
* m: Lock and unlock system mutexes (tests task context locks).

--------------------------------------------------------------------------------
3. Compilation Instructions
--------------------------------------------------------------------------------
Run the automated script to build and pack the binary:
  build_OsUartPmTest.bat

* Output Binary:
  E:\projects\DevelopmentLevelCode\Neoway_N706B\N706B-A07-STD-OE_CN1X_ITRI-009_SDK\N706B-A07-STD-OE_CN1X_ITRI-009_SDK\out\bin\nwy_open_app.bin

* Release Zip:
  release/nwy_open_app.zip (Ready for flashing with the Aboot tool)
