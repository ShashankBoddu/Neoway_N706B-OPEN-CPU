# Neoway N706B Mini EVB: Dedicated Multi-Port UART Test Harness (`Uart_Test`)

## 1. Overview & Purpose
This project is an isolated, robust test harness for testing UART communication on the **Neoway N706B-CN-10** module using the **Neoway Mini EVB (N706-CB_Mini_EVB_V1.0)**.

To eliminate any ambiguity about port name mappings, this test harness initializes and drives the operational hardware UARTs:
- **`"URT1"`**: Module Pins 38 (`DBG_RXD`) & 39 (`DBG_TXD`) $\to$ Connected to EVB Header 13 (`J607`, labeled `UART4` on PCB).
- **`"URT2"`**: Module Pins 17 (`MAIN_TXD`) & 18 (`MAIN_RXD`) $\to$ Connected to EVB Header 11 (`J606`, labeled `UART1` on PCB).

> [!NOTE]
> **AUX UART (`URT3` / Pins 28 & 29) Status:** On N706B modules with GNSS (`N706B-CN-10`), `URT3` is internally dedicated to the GNSS chip and is unavailable for application serial communication. It has been completely disconnected and removed from the active firmware.

Every 1 second, heartbeat pings are transmitted out of active ports and mirrored over USB CDC!

---

## 2. Root Cause Analysis: Why Nothing Printed After `PC : 0x80018000`

When the secondary bootloader (BOOT33) finishes, it transfers control to the RTOS entry point:
```
[BOOT33]VB_VERSION_DATE :[20250211]
 ** PC : 0x80018000
```
Two issues caused the system to stop printing:

1. **Mandatory Non-Zero `event_count` in `nwy_thread_create` (DEVELOPMENT_GUIDE.md §2-K):**
   - The SDK's FreeRTOS wrapper strictly requires `event_count > 0` (e.g. 50).
   - Passing `event_count = 0` causes `nwy_thread_create()` to fail immediately with `NWY_FAIL` (-1). Because the thread was not created, `main_monitor_task` was never executed!
   - **Resolution:** Set `event_count` to `50` in [`main.c`](file:///e:/projects/DevelopmentLevelCode/Neoway_N706B/Code/Uart_Test/src/main.c).

2. **Cortex-R5 MPU Memory Protection on Direct Address Access:**
   - Attempting to write directly to kernel RAM (`0x7e22e3c0`) from user application code triggers an ARM Cortex-R5 Data Abort exception, halting the core.
   - **Resolution:** Removed manual memory writes; use clean, standard SDK functions via [`hal_uart.c`](file:///e:/projects/DevelopmentLevelCode/Neoway_N706B/Code/Uart_Test/src/hal_uart.c).

---

## 3. EVB Header 12 (`J600`) Physical Pinout & Wiring

> [!WARNING]
> **Schematic Net Label Reversal Trap:**
> On the EVB schematic, connector pins are named from the perspective of external equipment:
> - **Pin 2** is labeled `UART2_RXD` $\to$ This is routed directly from **Module Pin 29 (`AUX_TXD`, Transmit Output)** through level translator `U600`.
> - **Pin 3** is labeled `UART2_TXD` $\to$ This is routed directly into **Module Pin 28 (`AUX_RXD`, Receive Input)** through level translator `U600`.
>
> **Therefore, to read serial data on your PC:**
> - Connect your USB-to-UART adapter's **RXD** wire to **Pin 2** of Header 12! (If connected to Pin 3, you are probing an input line and will see nothing).

### Header 12 Connection Matrix:
| Header 12 Pin | Silk / Net Name | Function / Module Pin | USB-to-UART Adapter Pin |
|---|---|---|---|
| **Pin 1** | `VDD_3P3` | 3.3V Power Out (from LDO `U603`) | *Do not connect / VCC* |
| **Pin 2** | `UART2_RXD` | **Module TX Output (Pin 29, `AUX_TXD`)** | **Connect to Adapter RXD** |
| **Pin 3** | `UART2_TXD` | **Module RX Input (Pin 28, `AUX_RXD`)** | **Connect to Adapter TXD** |
| **Pin 4** | `GND` | Ground Reference | **Connect to Adapter GND** |

> [!IMPORTANT]
> **Slide Switch Requirement:**
> Level translator `U600` is powered by the `VDD_3P3` rail. Slide switch **`J602` (Item 8 next to DC5V)** MUST be switched to **ON** to provide 3.3V power to the level translator.

---

## 4. Test Harness Output Format

Every 1 second, each active port emits its dedicated ping:
- **Header 13 (`URT1` / Pins 38 & 39 - DBG):**
  ```
  [URT1_DBG #0001] Ping! VBAT=3916 mV (RX=0)
  ```
- **Header 11 (`URT2` / Pins 17 & 18 - MAIN):**
  ```
  [URT2_MAIN #0001] Ping! VBAT=3916 mV (RX=0)
  ```
- **Header 12 (`URT3` / Pins 28 & 29 - AUX):**
  ```
  [URT3_AUX #0001] PING OUT ON PIN 29 (AUX_TXD)! VBAT=3916 mV (RX=0)
  ```

### Interactive Echo & Commands
Typing into any port triggers an immediate echo on that port and executes single-character commands:
- **`v`** $\to$ Read battery voltage (`VBAT` in mV)
- **`t`** $\to$ Read RTC calendar time
- **`r`** $\to$ Reboot module
- **`h`** $\to$ Show command menu
