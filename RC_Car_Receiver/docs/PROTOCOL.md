# Wire Protocol & Data Structures

This document describes the binary wire protocol between the controller (Sender) and the car (Receiver). The sender implementation lives in `Xbox_USB_HID_Communication/main/` and the receiver implementation lives in `RC_Car_Receiver/main/`.

The canonical definitions live in `common/shared_protocol.h`, and both sender and receiver must use the same layout. That shared header is the integration point between the two projects.

Control packet (Sender -> Receiver)
- Struct name: `control_packet_t` (packed)
- Fields and semantics (in order):
  - `joy_lx` (uint8_t): Left joystick X, range 0..255.
  - `joy_ly` (uint8_t): Left joystick Y, range 0..255.
  - `joy_rx` (uint8_t): Right joystick X, range 0..255.
  - `joy_ry` (uint8_t): Right joystick Y, range 0..255.
  - `trigger_l` (uint16_t): Left analog trigger (raw ADC value or normalized 0..65535 depending on sender).
  - `trigger_r` (uint16_t): Right analog trigger.
  - `buttons` (uint16_t): Bitmask representing buttons; bit assignments are project-specific and must match the sender.

Interpretation guidelines
- Joystick center: typically ~128. Map to signed range before applying control logic: centered → 0, left/up → negative, right/down → positive.
- Triggers: if sender provides 0..1023 values, they may be scaled up/down — receiver should be robust to range differences or expect the agreed range.
- Buttons: define constants in a shared header or enum so both ends interpret bits the same way.

Telemetry packet (Receiver -> Sender)
- Struct name: `telemetry_packet_t` (packed)
- Fields and semantics (in order):
  - `battery_voltage` (float): Battery voltage measured by ADC and converted to volts.
  - `distance_cm` (uint16_t): Distance reading from the proximity sensor in centimeters.
  - `status_flags` (uint8_t): Status bits (lights, alarm, errors). Define bits centrally.

Versioning and compatibility
- The protocol is not self-describing. When changing fields or sizes, bump a protocol version in a shared header and implement compatibility handling.

Example: mapping joystick to motor output (pseudo)
- Read `joy_ly` (0..255) → convert to signed range: signed = (joy_ly - 128)
- Normalize: norm = signed / 127.0  // range approximately -1.0..1.0
- Motor PWM duty = clamp((norm * max_speed), min, max)

See code references
- Packet definitions: [common/shared_protocol.h](../common/shared_protocol.h#L1)
- Receiver logic: [RC_Car_Receiver/main/esp_now_receiver.c](../main/esp_now_receiver.c#L1)
- Sender logic: [Xbox_USB_HID_Communication/main/main.c](../../Xbox_USB_HID_Communication/main/main.c)

Runtime behavior and frequencies
- Sender -> Receiver: control packets are transmitted at ~50Hz (every 20ms) by the USB/HID sender task. The sender also registers an ESP-NOW receive callback to accept telemetry packets from the receiver.
- Receiver loop: the receiver main loop refreshes its console/status output at 100Hz (every 10ms). This guarantees the latest known controller state is always printed even if a new packet didn't arrive that exact cycle.
- LED & change detection: the receiver compares each incoming `control_packet_t` with the previous packet; on difference it lights a status LED briefly (~150ms) to indicate a change.
- Telemetry: the receiver sends `telemetry_packet_t` back to the sender periodically (the current firmware sends it approximately every 1 second). The telemetry packet contains `battery_voltage`, `distance_cm`, and a `status_flags` byte.

Receiver proximity hardware
- The proximity visualization samples ADC1 channel 6 on GPIO34 and ADC1 channel 7 on GPIO35.
- The higher voltage reading from the two 0-3.3 V inputs controls the same filtered proximity value and drives the WS2812 strip data output on GPIO5.
