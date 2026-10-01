# DTRDesk Open Hardware

Open-source ESP8266 biometric attendance terminal hardware and firmware.

This repository contains the device firmware, local configuration template, and enclosure assembly documentation. Production services, deployment configuration, databases, and administrative controls are maintained separately.

> This is a **sanitized snapshot** published from the private production
> monorepo. Firmware fixes are made upstream and mirrored here; pinout, offline
> buffering, enrollment, NTP clock, TLS validation, captive-portal provisioning
> and OLED/TFT behavior are otherwise identical.
>
> **Configuration:** credentials and service URLs come from `firmware/.env`,
> which is compiled into `src/build_config.h` — never commit it. The
> self-provisioning endpoints default to generic placeholders; set
> `DTRDESK_CLAIM_URL` / `DTRDESK_REGISTER_URL` / `DTRDESK_ENROLL_RESULT_BASE` in
> `.env` to point them at your own service.

## Runtime device provisioning

One generic binary can be flashed to many devices — no secret is baked into the
image. Device identity (Device ID, one-time API key, API/sync URLs) is entered at
runtime and stored in LittleFS (`/device_config.json`) by `src/DeviceConfig.h`;
precedence is **runtime file > compile-time `build_config.h`**.

On first boot (or when Wi-Fi fails / the key is rejected) the device starts the
**`DTRDesk-Setup`** access point. The captive portal:

- **scans nearby Wi-Fi** so you pick a network from a list (password only if
  secured), auto-launches via a DNS redirect, and truncates long SSIDs;
- links the gateway to an organization via **Org ID + admin login**
  (`POST /devices/register`), or a **claim code** (`POST /devices/claim`) under
  *Advanced*; both are exchanged for the device's own API key;
- shows the AP credentials on the display: SSID `DTRDesk-Setup`, password
  **`dtr` + the last 5 hex digits of the chip id** (e.g. `dtr1a2b3`).

The device tolerates failure safely: wrong or corrupt Wi-Fi re-opens the portal,
a claim that keeps failing re-opens it after 10 tries, and a revoked/rotated key
stops cloud work and shows `REVOKED`.

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

## Display options

Two builds are provided:

- `pio run` (default) — 0.96" **SSD1306 OLED** (128×64, I²C on D1/D2).
- `pio run -e nodemcuv2_tft` — 2.8" **ST7789 240×320 SPI TFT**.

The TFT takes the hardware SPI bus, so the whole terminal is remapped (the OLED
is retired). All nine usable NodeMCU GPIOs are used:

| Signal | NodeMCU | Notes |
| :--- | :--- | :--- |
| TFT SCK / MOSI / CS / DC | D5 / D7 / D8 / D1 | hardware SPI |
| TFT RESET / backlight | 3V3 | software reset; backlight always on |
| TFT VCC | **VIN (5 V)** | board has a 3.3 V LDO — do not feed 3.3 V |
| AS608 RX / TX | D2 / D6 | SoftwareSerial |
| Green / Red LED | D0 / D4 | red is **active-low** (keeps GPIO2 high at boot) |
| Buzzer | D3 | unchanged |

Touch and the microSD slot are intentionally not wired: they share the SPI bus
and need an extra chip-select that the pin budget no longer has. If the TFT
image is negative or garbled, add `-D TFT_INVERSION_ON=1`, switch
`ST7789_DRIVER` to `ILI9341_DRIVER`, or lower `SPI_FREQUENCY` to 20 MHz.

## Hardware

The firmware targets a NodeMCU ESP8266 with an AS608 fingerprint sensor, an
SSD1306 OLED (or the 2.8" TFT above), status LEDs, and buzzer. See
[firmware/enclosure/README.md](firmware/enclosure/README.md) for wiring and
assembly details.
