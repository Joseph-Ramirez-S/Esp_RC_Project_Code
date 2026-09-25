# Xbox_USB_HID_Communication

This `docs` directory contains architecture, build, and development documentation for the Xbox USB HID host project.

- **Architecture and design:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **Build & flash instructions:** [BUILD_FLASH.md](BUILD_FLASH.md)
- **Development guide:** [DEVELOPMENT_GUIDE.md](DEVELOPMENT_GUIDE.md)
- **AI agent instructions:** [AI_AGENT_INSTRUCTIONS.md](AI_AGENT_INSTRUCTIONS.md)

## Purpose

This project reads an Xbox controller using the ESP32 USB host stack, translates controller input into a packed `control_packet_t`, and sends that packet via ESP-NOW to a remote receiver.

The firmware also supports sending RGB LED color commands and rumble commands to the Xbox controller via USB OUT transfers.
