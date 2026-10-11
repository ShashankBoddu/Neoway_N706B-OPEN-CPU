# Network_Mqtt_Rs485_DeepSleep Benchmark Application

## Overview
This production test application benchmarks the real-world operational current consumption profile of the **Neoway N706B (ASR1605S Cat-1)** module on the custom carrier board.

It cyclically performs the standard IoT operational loop:
1. **Peripheral Power On & RS-485 Probe**: Powers the `THVD1420DR` transceiver (`U14`), transmits a test probe packet at 9600 baud, and listens for 3 seconds for a response.
2. **Cellular Attachment & Data Call**: Attaches to LTE, reads signal quality (CSQ) and network operator, and starts a packet data call on CID 1.
3. **MQTT Telemetry Stream**: Connects to `mqtt.fervidlabs.in:1883`, publishes a structured JSON telemetry payload to `nwy_n706b/fervid/tx`, and gracefully terminates the session.
4. **Sub-Milliamp Deep Sleep**: Shuts down RF (`AT+CFUN=0`), cuts USIM LDO power, enforces baseband 32 kHz clock gating (`AT+ENPWRSAVE=1`), isolates all exposed carrier GPIOs, clamps transceiver power rail to 0V (`GPIO0=0`), and enters Cortex-R5 PMU sleep for 30 seconds.
5. **Periodic Repetition**: Automatically wakes up via internal timer after 30 seconds and repeats continuously for long-duration Nordic PPK2 power profiling.

---

## Hardware Pin Mapping & Circuit Architecture

### 1. RS-485 Transceiver (`THVD1420DR`, `U14`)
| Function | Module Pin | Pin Name | Circuit Description |
| :--- | :--- | :--- | :--- |
| **Power Switch (VCC)** | Pin 83 | `GPIO00` | Drives `Q5` (BC847B NPN) $\to$ `Q10` (BSS84AK P-MOSFET with 26k ohm pull-up).<br>• `HIGH` (1.8V): Powers `U14` VCC (3.916V) and `VCC_SWout` (5.45V).<br>• `LOW` (0.0V): Cuts power to `U14` VCC (0.32V) and `VCC_SWout` (0.00V). |
| **Direction Control (`RS_De`)** | Pin 87 | `GPIO87` | Drives `Q7` (BSS138BK N-MOSFET level shifter) connected to `U14` pins 2 (`RE#`) and 3 (`DE`).<br>• `HIGH` (1.8V): `Q7` OFF $\to$ `RS_De` pulled to `VBAT` via R36 (26.1k) $\to$ **TRANSMIT MODE** (`DE=1`, `RE#=1`).<br>• `LOW` (0.0V): `Q7` ON $\to$ `RS_De` pulled to GND $\to$ **RECEIVE MODE** (`DE=0`, `RE#=0`). |
| **UART TX** | Pin 17 | `MAIN_TXD` / `GPIO[4]` | Transceiver Pin 4 `D` $\to$ Level shifter $\to$ `URT2` TX on module. |
| **UART RX** | Pin 18 | `MAIN_RXD` / `GPIO[5]` | Transceiver Pin 1 `R` $\to$ Level shifter $\to$ `URT2` RX on module. |

### 2. Diagnostic & Debug Port
- **Port**: `URT1` (Pin 38 `DBG_RXD`, Pin 39 `DBG_TXD`)
- **Baud Rate**: 115200 baud, 8-N-1

---

## MQTT Configuration & Payload Format

- **Broker**: `mqtt.fervidlabs.in:1883`
- **Username**: `fervid`
- **Password**: `Fervid@123`
- **Topic**: `nwy_n706b/fervid/tx`
- **QoS**: 1

### Sample Published JSON Payload
```json
{
  "device": "N706B_ASR1605S",
  "cycle": 1,
  "vbat_mv": 3916,
  "csq": 24,
  "operator": "JIO 4G (405861)",
  "ip": "100.92.14.82",
  "rs485_tx": "N706B_RS485_CYCLE_1_TEST",
  "rs485_rx": "NO_RESPONSE_TIMEOUT"
}
```

---

## Power Benchmark Profile (Nordic PPK2)

| Stage | Activity | Expected Duration | Verified Average Current |
| :--- | :--- | :--- | :--- |
| **Stage 1: RS-485 Test** | Transceiver active, URT2 TX, 3s listen | ~3.2 s | 25 mA – 45 mA |
| **Stage 2: Cellular Attach** | LTE registration & CID 1 data call | ~5 s – 15 s | 35 mA – 80 mA |
| **Stage 3: MQTT Publish** | Socket connect, TLS/TCP handshake, publish | ~2.5 s | 40 mA – 140 mA (RF burst) |
| **Stage 4: Deep Sleep** | `AT+CFUN=0`, USIM LDO off, `GPIO0=0`, PMU sleep | 30.0 s | **~568.69 uA** (Record low floor) |

---

## Building and Flashing

### 1. Build the Binary
From the command prompt or terminal:
```cmd
cd e:\projects\DevelopmentLevelCode\Neoway_N706B\Code\Network_Mqtt_Rs485_DeepSleep
build_Network_Mqtt_Rs485_DeepSleep.bat
```

### 2. Output Artifacts
- **Aboot Download ZIP**: `release\nwy_open_app.zip` (for flashing via Neoway Upgrade Tool or Aboot over USB)
- **FOTA Packet**: `release\nwy_open_app_fota.pkt` (for local or HTTP FOTA differential flashing)
