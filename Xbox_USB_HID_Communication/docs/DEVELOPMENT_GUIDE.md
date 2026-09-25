# Development Guide

## Project structure

- `CMakeLists.txt` - ESP-IDF project definition.
- `main/main.c` - USB host initialization, controller parsing, and application loop.
- `main/esp_now_sender.c` - ESP-NOW initialization and periodic transmit task.
- `main/esp_now_sender.h` - public ESP-NOW API; includes `../../common/shared_protocol.h` for packet definitions.
- `common/shared_protocol.h` - shared packet definitions for both sender and receiver.

## How to read the code

1. Start at `main/main.c`.
   - `app_main()` initializes the system and enters the main event loop.
   - `client_event_cb()` handles USB device attachment and teardown.
   - `in_transfer_cb()` parses Xbox HID reports.
2. Review `process_joystick()` to understand joystick deadzone and scaling.
3. Inspect `set_controller_rgb()` and `send_rumble()` for USB OUT command construction.
4. Open `main/esp_now_sender.c` to see how ESP-NOW is configured and how telemetry is received.

## Adding new controller behavior

- To add a new button effect, update `in_transfer_cb()` with the new button bit and add the action inside the button-handling section.
- To add new color patterns, modify `target_r`, `target_g`, and `target_b` assignments.
- To add rumble profiles, update `send_rumble()` payload bytes and call it from the button logic.

## Wiring controller state into ESP-NOW packets

The current project has a `control_packet_t send_data_to_receiver` global and `esp_now_sender_update_data()` function.

To connect parsed input to ESP-NOW:

1. Build a `control_packet_t` instance inside `in_transfer_cb()` after parsing input.
2. Set joystick, trigger, and button values.
3. Call `esp_now_sender_update_data(&packet)`.

Example:

```c
control_packet_t packet = {0};
packet.joy_lx = joy_l_x;
packet.joy_ly = joy_l_y;
packet.joy_rx = joy_r_x;
packet.joy_ry = joy_r_y;
packet.trigger_l = trigger_l2;
packet.trigger_r = trigger_r2;
packet.buttons = 0; // assign bitmask flags here
esp_now_sender_update_data(&packet);
```

## Debugging tips

- Use `idf.py monitor` to view `ESP_LOG` messages.
- Add `ESP_LOGI()` lines in `in_transfer_cb()` to inspect button and axis values.
- If USB transfers fail, verify that the controller is attached and that the endpoint addresses match the device descriptor.
- If ESP-NOW fails, check that the receiver MAC address in `app_main()` matches the actual peer.

## Recommended improvements

- Move HID report parsing into a dedicated parser function.
- Add a proper button mask definition `#define BUTTON_A ...` and use named constants.
- Use a circular buffer or queue if the USB input rate becomes too high for the main loop.
- Separate USB and ESP-NOW state into modules with clear responsibilities.
