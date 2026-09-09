# DTRDesk Open Hardware

Open-source ESP8266 biometric attendance terminal hardware and firmware.

This repository contains the device firmware, local configuration template, and enclosure assembly documentation. Production services, deployment configuration, databases, and administrative controls are maintained separately.

## Build firmware

1. Install PlatformIO Core.
2. Copy `firmware/.env.example` to `firmware/.env`.
3. Fill in the device Wi-Fi and service values.
4. Run:

```bash
cd firmware
pio run
```

The build generates a private configuration header from `.env`; do not commit that file.

## Hardware

The firmware targets a NodeMCU ESP8266 with an AS608 fingerprint sensor, SSD1306 OLED, status LEDs, and buzzer. See [firmware/enclosure/README.md](firmware/enclosure/README.md) for wiring and assembly details.
