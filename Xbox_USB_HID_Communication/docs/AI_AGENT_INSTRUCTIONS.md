# AI Agent Instructions

## Purpose

This file contains instructions for an AI agent that is assisting with the `Xbox_USB_HID_Communication` project.

## Scope

- Provide architecture explanations for the USB host and ESP-NOW interaction.
- Document the HID report parsing logic, LED color command flow, and rumble command flow.
- Keep descriptions aligned with the current code in `main/main.c` and `main/esp_now_sender.c`.
- Keep updating this documentation whenever the project code or logic changes.

## Task guidance

1. Explain how the ESP32 acts as a USB host for an Xbox controller.
2. Describe how HID input reports are parsed from the controller and how button/axis values are extracted.
3. Describe how the controller is commanded with USB OUT transfers for RGB lighting and rumble.
4. Note the current implementation status: parsed controller input is logged and RGB/rumble actions are active, while the ESP-NOW packet update path from parsed input is present in the interface but not complete.
5. Keep the explanation concise and avoid inventing hardware details not present in the source.

## Formatting rules

- Use markdown headings and bullet points.
- Reference file names exactly: `main/main.c`, `main/esp_now_sender.c`, `main/esp_now_sender.h`, `common/shared_protocol.h`.
- Do not claim the project uses a UI or unused hardware unless it appears in the source.

## Example prompt for the AI agent

> Describe the architecture of the `Xbox_USB_HID_Communication` project, including how the Xbox controller is read over USB, how input is converted into ESP-NOW packets, and how controller RGB and rumble are driven.
