# Build & Flash

## Prerequisites

- ESP-IDF installed and configured.
- USB host-compatible ESP32-S3 board. The classic ESP32 target does not provide the USB OTG hardware required by `usb/usb_host.h`.
- Xbox controller connected to the ESP32 USB host port.
- The remote receiver MAC address set correctly in `main/main.c`.

## Build

From the `Xbox_USB_HID_Communication` project root:

```bash
d:
cd "c:\Users\Joseph.R\Documents\Esp_test_Projects\Xbox_USB_HID_Communication"
idf.py build
```

## Flash

```bash
idf.py -p <PORT> flash
```

Replace `<PORT>` with the serial port for your ESP32 board.

## Monitor

```bash
idf.py -p <PORT> monitor
```

Use monitor output to verify USB host initialization, controller detection, and ESP-NOW startup logs.

## Notes

- The USB host stack must be installed successfully before the controller can be opened.
- The first USB OUT packet is a 5-byte wake-up command sent when the controller is detected.
- The ESP-NOW remote receiver MAC is defined in `main/main.c` as `mac_RC_receiver`.
