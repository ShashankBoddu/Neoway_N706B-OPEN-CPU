# Network_Mqtt_Rs485_DeepSleep Benchmark Application

## Overview
This production application benchmarks and runs the operational IoT telemetry loop for the **Neoway N706B (ASR1605S Cat-1)** module interfacing with an industrial **RS-485 Hydrostatic Water Level Sensor** over Modbus RTU:

1. **Hydrostatic Sensor Power-Up & Modbus RTU Read**:
   - Enables `VCC_SWout` switched power rail (`GPIO00=1`) to power the sensor and `THVD1420DR` transceiver (`U14`) at **4800 baud** (8-N-1).
   - Waits **3000 ms** for transducer stabilization.
   - Transmits Modbus RTU command: `[0x01, 0x03, 0x00, 0x04, 0x00, 0x01, CRC_L, CRC_H]` (Slave: `0x01`, Func: `0x03`, Reg: `0x0004`).
   - Reverts to RX mode (`GPIO87=0`), parses raw 16-bit register value, converts to millimeters (`level_mm`) and meters (`level_m`).
   - Immediately powers OFF the sensor and transceiver (`GPIO00=0`) to preserve battery.
2. **Cellular Attachment & Data Call**: Attaches to LTE, reads signal quality (CSQ) and network operator, and starts a packet data call on CID 1.
3. **MQTT Telemetry Stream**: Connects to `mqtt.fervidlabs.in:1883`, publishes a structured JSON telemetry payload with water level and ADC metrics to `nwy_n706b/fervid/tx`, and gracefully terminates the session.
4. **Sub-Milliamp Deep Sleep**: Shuts down RF (`AT+CFUN=0`), cuts USIM LDO power, enforces baseband 32 kHz clock gating (`AT+ENPWRSAVE=1`), isolates all exposed carrier GPIOs, clamps transceiver power rail to 0V (`GPIO0=0`), and enters Cortex-R5 PMU sleep for 30 seconds.
5. **Periodic Repetition**: Automatically wakes up via internal timer after 30 seconds and repeats continuously for long-duration Nordic PPK2 power profiling.

---

## Hardware Pin Mapping & Circuit Architecture

### 1. RS-485 Transceiver (`THVD1420DR`, `U14`) & Hydrostatic Sensor
| Function | Module Pin | Pin Name | Circuit Description |
| :--- | :--- | :--- | :--- |
| **Power Switch (VCC)** | Pin 83 | `GPIO00` | Drives `Q5` (BC847B NPN) $\to$ `Q10` (BSS84AK P-MOSFET with 26k ohm pull-up).<br>• `HIGH` (1.8V): Powers `U14` VCC and `VCC_SWout` switched sensor rail.<br>• `LOW` (0.0V): Cuts power to `U14` and sensor (0.00V, battery preserved). |
| **Direction Control (`RS_De`)** | Pin 87 | `GPIO87` | Drives `Q7` (BSS138BK N-MOSFET level shifter) connected to `U14` pins 2 (`RE#`) and 3 (`DE`).<br>• `HIGH` (1.8V): `Q7` OFF $\to$ `RS_De` pulled to `VBAT` $\to$ **TRANSMIT MODE** (`DE=1`, `RE#=1`).<br>• `LOW` (0.0V): `Q7` ON $\to$ `RS_De` pulled to GND $\to$ **RECEIVE MODE** (`DE=0`, `RE#=0`). |
| **UART TX** | Pin 17 | `MAIN_TXD` / `GPIO[4]` | Transceiver Pin 4 `D` $\to$ Level shifter $\to$ `URT2` TX on module. |
| **UART RX** | Pin 18 | `MAIN_RXD` / `GPIO[5]` | Transceiver Pin 1 `R` $\to$ Level shifter $\to$ `URT2` RX on module. |
| **Sensor Protocol** | - | `Modbus RTU` | 4800 Baud, 8 Data Bits, 1 Stop Bit, No Parity (8-N-1). Slave: `0x01`, Register: `0x0004`. |

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

### Published JSON Payload Schema
```json
{
  "device": "N706B_ASR1605S",
  "cycle": 1,
  "vbat_mv": 3916,
  "csq": 24,
  "operator": "JIO 4G (405861)",
  "ip": "100.92.14.82",
  "adc1_raw_mv": 1200,
  "adc1_vin_mv": 3300,
  "adc0_raw_mv": 450,
  "adc0_current_ma": 12.50,
  "water_level_mm": 1250.0,
  "water_level_m": 1.250,
  "raw_register": 1250,
  "modbus_status": "OK"
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
