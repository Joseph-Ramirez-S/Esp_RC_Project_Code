# Architecture & Logic

## Overview

The `Xbox_USB_HID_Communication` firmware is an ESP32-based USB host application that:

- Detects and opens an Xbox-compatible USB controller.
- Reads HID input reports from the controller.
- Parses button, trigger, and joystick data.
- Sends controller-derived commands over ESP-NOW to a remote receiver.
- Sends USB OUT commands to the controller for LED color updates and rumble.

## Key components

- `main/main.c`
  - Entry point `app_main()`.
  - Initializes USB host and ESP-NOW.
  - Handles USB device connect/disconnect events.
  - Processes color update requests in the main loop.

- `main/esp_now_sender.c`
  - Initializes the ESP-NOW radio.
  - Registers the remote receiver peer.
  - Starts a periodic transmit task.
  - Receives telemetry from the remote device.

- `main/esp_now_sender.h`
  - Public interface for ESP-NOW initialization.
  - Public function to update the packet payload.
  - Includes `../../common/shared_protocol.h` so packet definitions stay shared.

- `common/shared_protocol.h`
  - Defines `control_packet_t` and `telemetry_packet_t`.
  - Ensures both sender and receiver share the same binary packet layout.

## Data flow

1. `app_main()` initializes USB host and ESP-NOW.
2. When the Xbox controller is connected, `client_event_cb()` opens the device, claims interface 0, sends a wake-up packet, and sets up an IN transfer.
3. The controller continuously delivers HID reports to `in_transfer_cb()`.
4. `in_transfer_cb()` parses raw report bytes:
   - Button status bits.
   - D-pad state.
   - Left and right trigger values.
   - Left and right joystick raw axes.
5. Joystick raw values are normalized through `process_joystick()` into the 0..255 range with a deadzone around center.
6. Discrete button presses update RGB color targets and/or trigger a rumble command.
7. RGB commands are sent using `set_controller_rgb()`.
8. Rumble commands are sent using `send_rumble()`.

## Packet protocol

The project uses `control_packet_t` from `common/shared_protocol.h`:

- `uint8_t joy_lx` - left joystick X
- `uint8_t joy_ly` - left joystick Y
- `uint8_t joy_rx` - right joystick X
- `uint8_t joy_ry` - right joystick Y
- `uint16_t trigger_l` - left trigger
- `uint16_t trigger_r` - right trigger
- `uint16_t buttons` - button bitmask

This packed struct is the payload sent over ESP-NOW.

## USB HID handling

- USB OUT endpoint `0x02` is used for controller commands.
- USB IN endpoint `0x82` is used for controller input reports.
- The code assumes the controller reports on a 0x20 HID report type and that valid packets contain at least 18 bytes.
- A first 5-byte "magic" packet is sent on connect to wake or initialize the controller.

## ESP-NOW handling

- `esp_now_sender_init()` sets up the ESP-NOW stack and peer registration.
- A transmit task sends `current_tx_data` at 50 Hz.
- Incoming ESP-NOW telemetry is captured in `on_data_recv()`.

## Notes on current implementation

- The project currently parses controller input and logs state.
- Color update and rumble logic are implemented and active.
- The data path that writes parsed controller values into `control_packet_t` for ESP-NOW transmission is present in the interface but not fully wired in the current main loop snapshot.

## Extension points

- Add a function to populate `send_data_to_receiver` from parsed controller values and call `esp_now_sender_update_data()`.
- Add full button-bitmask mapping into `control_packet_t.buttons`.
- Add explicit telemetry display or state handling when ESP-NOW telemetry arrives.
