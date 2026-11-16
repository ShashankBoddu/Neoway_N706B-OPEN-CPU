# Neoway N706B Industrial eDRX Power Benchmark (India MNO)

An industrial-grade, ultra-low-power benchmark application for the **Neoway N706B (ASR1605S Cat-1)** module running OpenCPU, designed for 3GPP Rel-13 **eDRX (Extended Discontinuous Reception)** power profiling and listening state validation on Indian cellular networks (Airtel M2M / Commercial, Reliance Jio, and Vodafone Idea).

---

## 1. Key Objectives & Architecture

| Metric / Feature | Specification | Engineering Purpose |
| :--- | :--- | :--- |
| **Protocol Mode** | 3GPP Rel-13 eDRX (E-UTRAN Cat-1, AcT: 4) | Keeps cellular connection active while extending paging listen cycle up to 40.96s. |
| **Target Idle Current** | **~1.2 mA – 2.5 mA average** | Compared to standard LTE DRX (~10–15 mA) or active idle (~25 mA). |
| **Power Gating** | Master Rail Switch `GPIO00 = 0` (`VCC_SWout`) | Completely cuts power to external level shifters and RS-485 transceiver (0.0 µA leakage). |
| **Carrier Isolation** | Carrier lines `1, 5, 7, 57, 84, 87` set to `IN + PD` | Eliminates parasitic shoot-through leakage on carrier board pins. |
| **Baseband Power Save**| `AT+ENPWRSAVE=1` (32.768 kHz CP clock gating) | Shuts down master 26 MHz PLL between paging windows. |
| **CPU PMU Sleep** | `nwy_pm_state_set(NWY_PM_SLEEP_ENABLE)` | Cortex-R5 core enters deep sleep during paging wait intervals. |
| **Reliability / FSM** | Bounded timeouts, non-blocking loops, auto-APN | Never hangs, freezes, or gets stuck during network search or SIM negotiation. |

---

## 2. Indian MNO APN Auto-Detection Database

The firmware automatically inspects the SIM IMSI (`AT+CIMI`) and Operator Name (`AT+COPS?`) to select the optimal APN without requiring manual firmware modifications:

* **Airtel India (Commercial & M2M):**
  - IMSI Prefixes: `40449` (AP/Telangana), `40445` (Karnataka), `40410` (Delhi), `40492` (Mumbai), `40470`, etc.
  - Primary APN: `airtelgprs.com` | Fallback M2M: `airtelm2m.com`
* **Reliance Jio 4G:**
  - IMSI Prefixes: `405854`, `405855`, `405856`, `405857`, `405858`
  - Primary APN: `jionet`
* **Vodafone Idea (Vi):**
  - IMSI Prefixes: `40404`, `40420`, `40446`, `40486`
  - Primary APN: `portalnmms` | Fallback: `www`
* **BSNL Mobile:**
  - IMSI Prefixes: `40434`
  - Primary APN: `bsnlnet`

---

## 3. 3GPP eDRX Paging Cycles & Negotiation

### AT Command Sequence:
1. **Request eDRX Configuration:**
   ```text
   AT+CEDRXS=1,4,"0011"
   ```
   - Mode `1`: Enable eDRX.
   - AcT `4`: E-UTRAN (LTE Cat-1 WB-S1 mode).
   - Requested $T_{\text{eDRX}}$ `"0011"`: **40.96 seconds** (or fallback to `"0010"`: **20.48 seconds**).

2. **Inspect Network Grant:**
   ```text
   AT+CEDRXRDP
   ```
   - Response: `+CEDRXRDP: 4,"<NW_TeDRX>","<NW_PTW>"`
   - If granted by MME, firmware translates binary bits into exact seconds:
     - $T_{\text{eDRX}}$: Cycle interval between paging listening windows.
     - $\text{PTW}$: Paging Time Window duration (active radio reception window).
   - If MME rejects eDRX on the serving cell, firmware falls back gracefully to standard DRX without failing.

---

## 4. Hardware Power Profile & Benchmarking Guide

```
+-----------------------------------------------------------------------------------+
| BOOT & ATTACH PHASE (~20 - 45 mA baseline, RF spikes up to 0.45 A)                |
|  * Powers module, validates SIM card readiness, auto-detects APN                 |
|  * Enforces AT+ENPWRSAVE=2 (DTR HIGH = SLEEP) & pre-configures AT+CEDRXS before attach|
|  * Waits for CEREG attachment on LTE network (Home or Roaming)                    |
|  * Reads AT+CEDRXRDP and samples RF metrics (CSQ, RSRP, RSRQ)                     |
|  * Power-gates external carrier rail (GPIO00 = 0) and isolates carrier pins       |
|  * Prints initial industrial status card via UART1                                |
+-----------------------------------------------------------------------------------+
                                       |
                                       v
+-----------------------------------------------------------------------------------+
| RRC INACTIVITY RELEASE (~12 - 15 seconds)                                         |
|  * Closes UART1 debug port to allow peripheral clock gating                       |
|  * Network eNodeB completes RRC Inactivity Timer and sends RRCConnectionRelease   |
|  * UE transitions from RRC Connected (~20 mA) into RRC Idle (~1.5 mA)             |
+-----------------------------------------------------------------------------------+
                                       |
                                       v
+-----------------------------------------------------------------------------------+
| PURE STEADY-STATE eDRX/DRX LISTENING STATE (~1.2 mA - 1.8 mA baseline)            |
|  * Cortex-R5 CPU PMU enters deep sleep (NWY_PM_SLEEP_ENABLE)                      |
|  * Modem baseband CP operates on 32.768 kHz RTC clock (AT+ENPWRSAVE=2)            |
|  * External carrier components draw 0.0 uA leakage (GPIO00 = 0)                   |
|  * Radio wakes up ONLY during Paging Occasion / Paging Time Window (PTW)          |
|  * Zero periodic AT/IPC commands disturb the radio -> NO recurring 20 mA blocks!  |
+-----------------------------------------------------------------------------------+
```

---

## 5. Build & Flash Instructions

1. **Build Binary & Packages:**
   Run the automated build script from the project folder:
   ```cmd
   build_eDRX_Power_Benchmark.bat
   ```
2. **Generated Release Artifacts:**
   - **Aboot ZIP Package:** `release/nwy_open_app.zip` (for SWDownloader USB flashing)
   - **FOTA Update Packet:** `release/nwy_open_app_fota.pkt`
3. **Flashing:**
   - Open **SWDownloader**.
   - Load `release/nwy_open_app.zip`.
   - Boot into download mode and flash.
   - Open serial terminal on **URT1 (Pin 35/36)** at **115200 baud, 8-N-1** to observe benchmark metrics.
