# Neoway N706B OpenCPU — Industrial I2C EEPROM (P24C128D) Driver

Production-grade, highly robust, non-blocking I2C EEPROM storage subsystem for the **Neoway N706B** LTE Cat-1 OpenCPU module interfacing with the **Puya Semiconductor P24C128D-SSH-MIR** (128 Kbit / 16 KByte) I2C EEPROM.

---

## 1. Hardware Architecture & Schematics

### 1.1 Pinout & Wiring Table
| Module Pin | Name / Net | Native Controller | Pin Direction | Target U7 Pin | Function |
|:---:|:---:|:---:|:---:|:---:|:---:|
| **Pin 67** | `I2C2_SCL` | `GPIO 10` (I2C2 SCL) | Open-Drain / Output | Pin 6 (`SCL`) | Serial Clock (4.7kΩ R25 Pull-up to `VDD_EXT`) |
| **Pin 66** | `I2C2_SDA` | `GPIO 11` (I2C2 SDA) | Open-Drain / Bi-dir | Pin 5 (`SDA`) | Serial Data (4.7kΩ R26 Pull-up to `VDD_EXT`) |
| — | `VDD_EXT` | 1.8V Power Rail | Power Output | Pin 8 (`VCC`) | 1.8V Supply Voltage to U7 |
| — | `GND` | Ground | Ground | Pins 1, 2, 3, 4 | `E0=GND`, `E1=GND`, `E2=GND`, `GND` |
| — | `WCB` | Write Control | Passive Pull-down | Pin 7 (`WCB`) | Floating (Internal pull-down -> Writes Enabled) |

### 1.2 Device Addressing
- **7-Bit Slave Address**: `0b1010000` = `0x50`
  - 8-bit Write Address: `0xA0`
  - 8-bit Read Address: `0xA1`
- **P24C128D Security / ID Page Address**: `0b1011000` = `0x58`

---

## 2. Key Reliability & Industrial Features

### 2.1 9-Clock Bus Unstick Recovery (`i2c_bus_recover`)
If a brownout or reset occurs while an I2C slave is transmitting a `0` bit, the slave will hold `SDA` LOW indefinitely, locking up standard hardware I2C controllers.
- Before initializing the bus, `i2c_bus_init()` verifies the state of `SDA`.
- If `SDA` is held LOW, it clocks up to 9 clock pulses on `SCL` until the slave releases `SDA`, followed by a standard `STOP` condition.

### 2.2 Dual-Engine Execution Architecture (HW + Soft Bit-Bang Fallback)
- **Primary**: Native ASR1605 Crane `I2C2` hardware peripheral running at **400 kHz** Fast Mode.
- **Secondary / Fallback**: Fully deterministic, jitter-free software bit-bang engine on `GPIO 10` & `GPIO 11`.
- **Automatic Fallback (`I2C_MODE_AUTO`)**: If the hardware peripheral detects any bus anomaly or silicon errata, the driver transparently executes via the bit-bang engine with zero hangs or freezes.

### 2.3 Automatic 64-Byte Page Boundary Chunking
Standard 24Cxx EEPROMs roll over internally to the beginning of the same page if a write crosses a page boundary:
$$\text{Page Offset} = \text{Address} \pmod{64}$$
$$\text{Max Chunk} = 64 - \text{Page Offset}$$
The `eeprom_write()` function automatically calculates physical page boundaries and splits arbitrary-length transfers (e.g. 1 byte to 16,384 bytes) into page-aligned chunks, eliminating buffer roll-over corruption.

### 2.4 Non-Blocking Acknowledge Polling (ACK Polling)
Rather than executing blind `5 ms` blocking delays after every page write, the driver executes **Acknowledge Polling**:
- Immediately after a page write `STOP` condition, it polls the EEPROM address `0x50`.
- As soon as internal physical write cycles complete (typically $2\text{--}3\text{ ms}$, max $5\text{ ms}$), the EEPROM ACKs and the driver proceeds immediately.
- Includes a strict `10 ms` bounded safety timeout to prevent deadlocks.

### 2.5 Structured Config Storage with IEEE 802.3 CRC-32
Allows high-level application code to serialize arbitrary configuration or calibration structs:
- Wraps payload in `eeprom_record_header_t` (`[Magic: 4B][Version: 2B][Len: 2B][CRC32: 4B]`).
- On readback, automatically recomputes CRC-32 and verifies schema integrity.

### 2.6 P24C128D Factory 128-Bit Unique Serial Number
Direct support for reading the factory-programmed 16-byte unique identifier (`eeprom_read_serial_number()`) and 64-byte Identification Page (`eeprom_read_id_page()`).

---

## 3. Directory Layout

```
Code/EEPROM_I2C_Storage/
├── build_EEPROM_I2C.bat       # One-click Ninja compile & Aboot/FOTA packager
├── README.md                  # System & API documentation
├── inc/
│   ├── i2c_bus.h              # I2C bus driver, 9-clock recovery, recursive mutex
│   └── eeprom_24cxx.h         # P24C128D EEPROM API, page splitting, CRC32, ID page
├── src/
│   ├── i2c_bus.c              # Hardware & software I2C engines
│   ├── eeprom_24cxx.c         # Page-boundary chunking, ACK polling, CRC verification
│   └── main.c                 # Banner, 6-phase test suite, non-volatile heartbeat
└── release/
    ├── nwy_open_app.zip       # Complete Aboot flash package (USB download)
    └── nwy_open_app_fota.pkt  # FOTA update binary
```

---

## 4. API Reference

### 4.1 EEPROM Management
```c
/* Initialize EEPROM and low-level I2C bus */
int eeprom_init(const eeprom_config_t *cfg);

/* Deinitialize and release bus */
void eeprom_deinit(void);

/* Probe device presence */
bool eeprom_ping(void);

/* Read/Write byte */
int eeprom_read_byte(uint16_t mem_addr, uint8_t *val);
int eeprom_write_byte(uint16_t mem_addr, uint8_t val);

/* Read arbitrary buffer across page boundaries */
int eeprom_read(uint16_t mem_addr, uint8_t *data, uint16_t len);

/* Write arbitrary buffer with automatic 64-byte page splitting & ACK polling */
int eeprom_write(uint16_t mem_addr, const uint8_t *data, uint16_t len);

/* Write and readback verify */
int eeprom_write_verify(uint16_t mem_addr, const uint8_t *data, uint16_t len);

/* Erase / Format memory section */
int eeprom_fill(uint16_t start_addr, uint16_t len, uint8_t pattern);
```

### 4.2 Security & Identification
```c
/* Read P24C128D Factory 128-bit (16 bytes) unique serial number */
int eeprom_read_serial_number(uint8_t sn_buf[16]);

/* Read 64-byte Identification page */
int eeprom_read_id_page(uint8_t id_buf[64]);
```

### 4.3 Structured Config Storage (CRC-32)
```c
/* Write struct with magic and CRC-32 header */
int eeprom_write_config_record(uint16_t mem_addr, uint32_t magic,
                               const void *data, uint16_t len);

/* Read struct and validate CRC-32 integrity */
int eeprom_read_config_record(uint16_t mem_addr, uint32_t magic,
                              void *data, uint16_t max_len, uint16_t *actual_len);
```

---

## 5. Build & Flash Instructions

1. Double-click or execute in command line:
   ```cmd
   build_EEPROM_I2C.bat
   ```
2. Generated output packages:
   - **USB Aboot Package**: `release/nwy_open_app.zip`
   - **FOTA Binary**: `release/nwy_open_app_fota.pkt`
3. Flash `release/nwy_open_app.zip` to the Neoway N706B module using the Aboot download tool.
4. Open Debug UART1 at **115200 8N1** to view the live 6-phase test suite and non-volatile heartbeat counter.
