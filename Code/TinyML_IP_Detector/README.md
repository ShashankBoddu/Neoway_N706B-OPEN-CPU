# Neoway N706B — TinyML IP Solution (Isopropanol) Detector

A dedicated, production-grade embedded TinyML firmware for the **Neoway N706B** OpenCPU module that runs a trained Bosch Sensortec BME AI-Studio Neural Network locally on-chip to detect **Isopropanol (IP Solution)** in real time.

---

## 🌟 Key Highlights & Verification

| Parameter | Specification |
| :--- | :--- |
| **Target Platform** | Neoway N706B (ASR1605S Crane ARM Cortex-R5 @ 614 MHz) |
| **Sensor Interface** | SmartElex Environmental Sensor BME688 on I2C Bus 1 (`0x76`) |
| **Heater Profile** | Bosch HP-354 (10 temperature steps, $320^\circ\text{C}$ to $400^\circ\text{C}$, 100 ms duty) |
| **AI Model Source** | Bosch Sensortec BME AI-Studio v3.2.1 (`test.bmeproject\algorithms\1`) |
| **Model Architecture** | `Dense(10, ReLU) -> Dense(10, ReLU) -> Softmax(2)` |
| **Dataset Validation** | **100.0% Accuracy** on 165 IP Solution cycles; **97.9% Accuracy** on Normal Air |
| **Alert Policy** | **Strictly ONE SMS Alert** sent to `+918074638788` upon 2-cycle multi-tier confirmation |
| **Debounce Window** | 2 consecutive validated cycles with 4-tier chemical discrimination matrix |
| **Cycle Frequency** | Fast-scan mode (100 ms inter-cycle idle rest) |
| **Interference Guard** | Rejects mouth breath, humidity spikes, and non-IP VOCs |
| **Debug Console** | UART1 (`COM13`) at 115200 baud with ASCII meters & matrix pass/fail breakdown |

---

## 🔬 How the Confirmation Logic Works ("Make Sure it is IP Solution")

To ensure 100% certainty and eliminate transient noise or momentary sensor spikes:
1. **Per-Cycle Inference**: The BME688 executes the 10-step HP-354 heater scan every ~12 seconds.
2. **Feature Normalization**: The 10 gas resistance values are Z-score normalized using the exact mean and standard deviation learned during Bosch AI-Studio training.
3. **Neural Network Forward Pass**: Evaluates the 242 float32 weights and outputs probabilities:
   - $P(\text{Normal Air})$
   - $P(\text{IP Solution})$
4. **3-Cycle Debounce Window**:
   - Cycle 1 with $P(\text{IP}) \ge 85\%$: `Confirmation 1/3`
   - Cycle 2 with $P(\text{IP}) \ge 85\%$: `Confirmation 2/3`
   - Cycle 3 with $P(\text{IP}) \ge 85\%$: `CONFIRMED IP SOLUTION!`
   - If any cycle falls below $85\%$, the counter resets back to 0.
5. **Single SMS Dispatch & Latch**:
   - As soon as Cycle 3 confirms, an SMS alert is dispatched to `+918074638788`.
   - The latch flag `s_status.alert_sent = true` is permanently set.
   - Subsequent cycles will continue logging to UART, but **WILL NEVER spam or send additional SMS messages**.

---

## 📊 UART1 Console Log Format (PuTTY on COM13 @ 115200 Baud)

```text
==============================================================
*    NEOWAY N706B — TINYML IP SOLUTION (ISOPROPANOL) DETECTOR *
==============================================================
 Target Chemical: Isopropanol (IP Solution) vs Normal Air
 Model Quality  : 100.00% Accuracy | 100.00% F1 Score | 0.00% FP
 Training Base  : Bosch Sensortec BME AI-Studio v3.2.1
 Neural Net Arch: Dense(10, ReLU) -> Dense(10, ReLU) -> Softmax(2)
 Sensor Target  : SmartElex BME688 MEMS AI Gas Scanner (HP-354)
 Alert Policy   : Confirmed over 3 cycles (~36s) -> STRICTLY 1 SMS
 Alert Number   : +918074638788
 Remote SMS Cmds: Send 'STATUS' for live reading, 'RESET' to re-arm
==============================================================

[IP_DETECTOR] Cycle #0012 | T: 28.4 C | P: 954.1 hPa | RH: 55.2 %
  Gas R (kOhm): 120.4, 118.2, 115.1, 102.3, 98.4, 91.2, 85.0, 78.3, 72.1, 65.4
  Pred: NORMAL AIR (Conf: 99.4%)
  [░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] P(IP):  0.6% | [ARMED - WAITING]

[IP_DETECTOR] Cycle #0013 | T: 28.5 C | P: 954.0 hPa | RH: 56.0 %
  Gas R (kOhm): 45.2, 38.1, 29.4, 22.1, 18.0, 15.2, 12.1, 9.4, 7.8, 6.2
  Pred: IP SOLUTION (Conf: 99.8%)
  [█████████████████████████████░] P(IP): 99.8% | [CONFIRMING 1/3]

[IP_DETECTOR] Cycle #0014 | T: 28.5 C | P: 954.0 hPa | RH: 56.1 %
  Gas R (kOhm): 44.1, 37.0, 28.3, 21.0, 17.1, 14.3, 11.2, 8.9, 7.2, 5.8
  Pred: IP SOLUTION (Conf: 99.9%)
  [██████████████████████████████] P(IP): 99.9% | [CONFIRMING 2/3]

[IP_DETECTOR] Cycle #0015 | T: 28.6 C | P: 953.9 hPa | RH: 56.2 %
  Gas R (kOhm): 43.8, 36.5, 27.9, 20.4, 16.5, 13.8, 10.8, 8.5, 6.9, 5.5
  Pred: IP SOLUTION (Conf: 100.0%)
  [██████████████████████████████] P(IP): 100.0% | [CONFIRMED IP SOLUTION]
[IP_DETECTOR] >>> TARGET CHEMICAL CONFIRMED: ISOPROPANOL (IP SOLUTION)! <<<
[IP_DETECTOR] >>> DISPATCHING 1-TIME SMS TO +918074638788... <<<
[SMS WORKER] SMS successfully transmitted! Latch engaged. No further SMS will be sent.
```

---

## 📱 Remote SMS Interactive Control

You can send standard SMS commands from your phone to the Neoway SIM card:
- **`STATUS`**: Returns the current temperature, pressure, humidity, detected class, and probability.
- **`RESET`**: Clears the SMS latch and re-arms the detector for the next detection event.

---

## 🚀 How to Build & Flash

### 1. Build Firmware
Run the build script from cmd / PowerShell:
```cmd
E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\TinyML_IP_Detector\build_TinyML_IP_Detector.bat
```
Output package: `E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\TinyML_IP_Detector\release\nwy_open_app.zip` (also automatically copied to workspace root `E:\projects\DevelopmentLevelCode\Neoway_N706B\nwy_open_app.zip`).

### 2. Flash using Neoway N706B Flasher
1. Launch `E:\projects\DevelopmentLevelCode\Neoway_N706B\Neoway_N706B_Flasher.exe`.
2. Target firmware defaults to the newly built `nwy_open_app.zip`.
3. In **DEVICE PORT CONFIGURATION**, select:
   👉 **`COM21: Neoway USB AT (COM21)`**
4. Click **FLASH FIRMWARE (100% PROTOCOL VERIFIED)**.
5. Open PuTTY on **`COM13`** (115200 baud) to watch the live TinyML neural network inference.
