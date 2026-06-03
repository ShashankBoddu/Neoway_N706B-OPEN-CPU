# Neoway N706B OpenCPU Network Monitor

This project provides a robust, industrial-grade network monitoring and diagnostic application for the Neoway N706B (ASR1605 platform) OpenCPU SDK. It focuses on resilient network connectivity, automated self-healing, advanced diagnostics, and peripheral indications.

## 🚀 Key Features

### 1. Robust Network Diagnostics & Self-Healing
* **LTE Locking**: Automatically locks the module to LTE (0x10) to prevent the modem from pointlessly scanning 2G/3G bands on 4G-only carriers (e.g., Jio).
* **VoLTE Activation**: Automatically activates VoLTE / IMS services if disabled.
* **Auto-Recovery Supervisor**: Continuously monitors the CS/PS domain state. If the modem remains "Out of Service" for 60 consecutive seconds, it triggers a soft Flight Mode toggle (`AT+CFUN=4` -> `AT+CFUN=1`) to force a fresh network scan.
* **Signal Quality Analysis**: Translates raw CSQ values into approximate dBm ranges and provides advanced cellular parameters (RSRP, RSRQ, RSSI, SINR, EARFCN, PCI).

### 2. SIM Card Hot-Swap & Reliability
* **Software Hot-Swap Support**: Disables physical hardware pin detection (which custom boards often lack) and relies on software polling. If a SIM is removed and re-inserted, the auto-recovery supervisor seamlessly invokes a soft reset on the SIM stack (`nwy_hal_sim_reset`) to remount it without rebooting the module.

### 3. LED Hardware Indications (GPIO)
Provides visual status feedback via standard GPIOs (`GPIO 70` for Power Status, `GPIO 69` for Network Status):
* **Status LED (Solid ON)**: Application is running.
* **Network LED**:
  * *Fast Blink (5 Hz)*: Searching for network / Out of Service.
  * *Slow Blink (0.5 Hz)*: Registered to network, but data context is disconnected.
  * *Pulsing (Short 100ms ON, 2000ms OFF)*: Data context (Internet) is connected.

### 4. Automated SNTP Time Synchronization
* Upon successful GPRS PDP context activation and IP allocation, the application automatically triggers an SNTP request to `pool.ntp.org` to synchronize the modem's internal RTC. 
* Configured by default for the `E5` (East 5) timezone offset.

### 5. SMS Interaction (Echo & Boot Notification)
* **Boot Alert**: Sends an automated initialization SMS to a hardcoded number when the system successfully connects to the network.
* **Echo Response**: Automatically intercepts incoming SMS messages and echoes them back to the sender for easy connectivity testing.

## 📂 Project Structure

```text
NetworkMonitor/
├── build_NetworkMonitor.bat    # Windows build & packaging script
├── CMakeLists.txt              # CMake configuration
├── inc/
│   └── hal/
│       ├── gpio/nwy_hal_gpio.h # GPIO LED control abstraction
│       ├── net/nwy_hal_net.h   # Network diagnostic & modem controls
│       ├── pm/nwy_hal_pm.h     # Power management utilities
│       ├── sim/nwy_hal_sim.h   # SIM card utilities & resetting
│       ├── sms/nwy_hal_sms.h   # SMS transmission & reception
│       ├── sntp/nwy_hal_sntp.h # SNTP time sync abstraction
│       └── uart/nwy_hal_uart.h # UART debug logging abstraction
├── src/
│   ├── main.c                  # Main application thread & supervisor
│   └── hal/                    # Implementations for the HAL layer
└── release/
    └── nwy_open_app.zip        # Output Aboot flashable firmware
```

## 🛠️ Building and Flashing

1. Execute the `build_NetworkMonitor.bat` script located in the project root.
2. The script will clean old artifacts, invoke Ninja/CMake, and execute the Aboot packager.
3. Once completed successfully, flash the generated `release/nwy_open_app.zip` via the Neoway Aboot Flashing tool.
4. Monitor the debug output on **UART1 (Debug)** at a baud rate of `115200`.
