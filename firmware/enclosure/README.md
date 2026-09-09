# DTRDesk.com — Biometric Hardware Wall-Mount Enclosure & Assembly Guide

This document contains the complete mechanical specifications, 3D CAD dimensions, Bill of Materials (BOM), and step-by-step physical assembly guide for building the **DTRDesk ESP8266 + AS608 Biometric Time Clock Terminal**.

---

## 📐 1. 3D Enclosure Mechanical Blueprint

The enclosure is designed as a two-piece snap-fit / M3-bolted housing (Front Bezel + Wall Mounting Backplate) printable in PLA, PETG, or ABS with no supports needed.

```
+-------------------------------------------------------------+
|                     DTRDESK TERMINAL                        |
|                                                             |
|   +-----------------------------------------------------+   |
|   |         0.96" SSD1306 OLED (128x64 Pixels)          |   |
|   |                  [ 27mm x 15mm ]                    |   |
|   +-----------------------------------------------------+   |
|                                                             |
|           ( ) Green LED                 ( ) Red LED         |
|              [5mm]                         [5mm]            |
|                                                             |
|                         (((( * ))))                         |
|                      Buzzer Sound Port                      |
|                                                             |
|                     /=================\                     |
|                    /                   \                    |
|                   |   AS608 OPTICAL     |                   |
|                   |  FINGERPRINT SENSOR |                   |
|                   |     ( 22mm DIA )    |                   |
|                    \                   /                    |
|                     \=================/                     |
|                                                             |
|   [ Micro-USB / 5V DC Power Inlet ] (Bottom Center)         |
+-------------------------------------------------------------+
```

### Enclosure Dimensions & Tolerances

| Component Cutout | Dimensions | Placement on Face | Function |
| :--- | :--- | :--- | :--- |
| **Overall Outer Shell** | 95mm (W) x 135mm (H) x 28mm (D) | Outer Enclosure | Compact wall-mounted footprint |
| **OLED Display Window** | 27.5mm x 15.0mm (Beveled edge) | Top Center (Y=18mm) | Houses 0.96" SSD1306 I2C Screen |
| **AS608 Sensor Recess** | 22.2mm Diameter (15° ergonomic tilt) | Center-Bottom (Y=72mm) | Physical finger placement cradle |
| **Status LEDs** | Dual 5.1mm holes | Mid-Left / Mid-Right | Green (Grant) & Red (Deny/Offline) |
| **Acoustic Grille** | 4 x 1.5mm pinholes | Center (Y=52mm) | Piezo buzzer audio resonance |
| **Power Cable Slot** | 12mm x 6mm rectangular notch | Bottom Edge | Micro-USB or 5V/2A DC power wire |
| **Wall Mount Backplate** | 4 x M3 screw keyholes (60mm spacing) | Rear Plate | Universal drywall/masonry mount |

---

## 🛠️ 2. Bill of Materials (BOM)

| Part | Component | Description / Specification | Qty | Est. Cost (USD) |
| :---: | :--- | :--- | :---: | :---: |
| **U1** | **NodeMCU ESP8266 v3** | Wi-Fi Microcontroller (80MHz, 4MB Flash, LittleFS) | 1 | $3.50 |
| **U2** | **AS608 Optical Sensor** | 1,000 Fingerprint Template Database, UART 57600 baud | 1 | $7.80 |
| **U3** | **0.96" SSD1306 OLED** | 128x64 I2C Blue/White Monochrome Graphic Display | 1 | $2.20 |
| **D1** | **5mm Green LED** | Visual Success / Network Online indicator | 1 | $0.10 |
| **D2** | **5mm Red LED** | Visual Deny / Network Offline / Lockdown indicator | 1 | $0.10 |
| **R1, R2**| **220Ω Resistors** | 1/4W Through-hole current limiting for LEDs | 2 | $0.05 |
| **BZ1**| **Active 5V Piezo Buzzer** | 85dB Audio feedback chime | 1 | $0.35 |
| **ENC**| **3D Printed Enclosure**| PLA / PETG Filament (~80 grams, 0.2mm layer height) | 1 | $1.60 |
| **HW** | **M3 x 8mm Hex Screws**| Fasteners for securing PCB and Backplate | 4 | $0.30 |
| **PSU**| **5V / 2A Micro-USB Supply**| Regulated USB Wall Power Adapter | 1 | $3.00 |
| **TOTAL** | | **Complete Hardware Terminal Unit Cost** | | **~$19.00** |

---

## ⚡ 3. Hardware Schematic & Pinout Map

```
  NodeMCU ESP8266                    Peripherals & Sensors
+------------------+             +---------------------------+
| 3V3 / VIN (5V)   |------------>| AS608 VCC (5V)            |
| GND              |------------>| AS608 GND                 |
| D5 (GPIO14 - RX) |------------>| AS608 TX (White Wire)     |
| D6 (GPIO12 - TX) |------------>| AS608 RX (Green Wire)     |
+------------------+             +---------------------------+
| 3V3              |------------>| OLED 0.96" VCC (3.3V)     |
| GND              |------------>| OLED 0.96" GND            |
| D1 (GPIO5 - SCL) |------------>| OLED 0.96" SCL (I2C Clock)|
| D2 (GPIO4 - SDA) |------------>| OLED 0.96" SDA (I2C Data) |
+------------------+             +---------------------------+
| D7 (GPIO13)      |--[ 220Ω ]-->| Green LED (+) -> GND      |
| D8 (GPIO15)      |--[ 220Ω ]-->| Red LED (+)   -> GND      |
| D3 (GPIO0)       |------------>| Buzzer (+)    -> GND      |
+------------------+             +---------------------------+
```

---

## 🔧 4. Step-by-Step Physical Assembly Instructions

### Step 1: 3D Printing the Housing
1. Slice `dtrdesk_front_bezel.stl` and `dtrdesk_backplate.stl` in Cura / PrusaSlicer.
2. Recommended Print Settings:
   - **Material**: PETG or PLA+
   - **Layer Height**: `0.20mm`
   - **Infill**: `25% Gyroid` for structural rigidity.
   - **Walls / Perimeters**: `3 walls`.

### Step 2: Component Installation
1. **OLED Screen**: Push the 0.96" OLED display into the top window frame until it clicks against the retaining tabs. Secure with a drop of hot melt glue or M2 bracket.
2. **AS608 Fingerprint Sensor**: Insert the sensor head into the 22mm front circular recess. The sensor is keyed to sit at an upward 15-degree angle for natural thumb placement.
3. **LEDs & Buzzer**: Insert the Green and Red 5mm LEDs into the respective lens collars. Press the 5V active buzzer into the acoustic grille slot.

### Step 3: Wiring & NodeMCU Mounting
1. Cut hookup jumper wires to 60mm lengths.
2. Solder the 220-ohm current-limiting resistors to the LED anodes.
3. Plug the female Dupont headers onto the NodeMCU pins according to the pinout map above.
4. Fasten the NodeMCU into the internal mounting standoffs with 4x M3 screws.

### Step 4: Final Closing & Testing
1. Connect the micro-USB cable into the NodeMCU power port.
2. On initial power-up, the OLED will light up with the **DTRDesk.com** splash screen and double-beep.
3. Align the rear backplate and tighten the 4 outer corner screws.
4. Mount to the office wall using standard drywall anchors at a recommended height of **130cm – 140cm (52" – 55")** from the floor.
