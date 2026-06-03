# NetworkMonitor & Connectivity Tester (N706B)

## Overview
This project is an industrial-grade, modular OpenCPU application for the Neoway N706B cellular module. It implements a sequential "Stage-Gate" connectivity verification architecture. The application automatically initializes the hardware, manages the cellular network state, synchronizes system time, and validates TCP/IP connectivity via dynamic DNS resolution.

## Architecture
The project follows a modular HAL (Hardware Abstraction Layer) design pattern:
- **`hal/net`**: Manages PDP Context, GPRS dial-up, and cellular registration.
- **`hal/sntp`**: Handles system clock alignment via NTP over UDP.
- **`hal/socket`**: Provides a robust TCP client interface with asynchronous background monitoring threads.
- **`main.c`**: The orchestrator. It manages the sequential execution logic and failure-recovery transitions.

## Verification Sequence (The "Stage-Gate" Datapath)
The application strictly follows this verification chain:
1. **SIM Initialization**: Validates SIM presence and card status.
2. **Network Registration**: Locks to LTE (4G) and registers with the provider.
3. **GPRS Activation (PDP Context)**: Establishes a data tunnel and acquires a dynamic IP.
4. **SNTP Synchronization**: Synchronizes system RTC via UDP.
5. **TCP/IP Socket Test**: Dynamically resolves DNS for `google.com` and performs a secure TCP handshake to Port 80.

## Build Instructions
1. Ensure the Neoway SDK environment is set.
2. Run `build_TCP_IPSockets.bat` in the project root.
3. The resulting binary is generated in the `release/` directory.

## Current Status
- Connectivity: **Verified** (Dynamic DNS Resolution & TCP Handshake Successful).
- Status: Ready for application-level protocol development (HTTP/HTTPS/MQTT).