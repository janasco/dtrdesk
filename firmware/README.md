# DTRDesk ESP8266 firmware

## Build the browser flash artifact

Install PlatformIO Core, copy `.env.example` to `.env`, fill in the device values, then run:

```bash
cd firmware
pio run
```

This compiles the NodeMCU v2 firmware and copies the resulting
`.pio/build/nodemcuv2/firmware.bin`.

## Flash from Chrome

1. Open the local or approved HTTPS browser installer.
2. Connect the ESP8266 with a data-capable USB cable.
3. Select the serial port, allow the erase, and wait for the device to reboot.

The browser installer is a USB serial flasher; it does not use Wi-Fi OTA. Keep the
device powered and connected until the browser reports that installation is complete.
