# Architecture and Runtime Logic

## System Overview

The receiver is an ESP32 application composed of independent FreeRTOS tasks and hardware modules. The controller sends a packed `control_packet_t` over ESP-NOW. The receiver stores the newest valid packet, applies it to the vehicle outputs, and periodically returns a packed `telemetry_packet_t`.

## Runtime Tasks

### Main control task

`main.c` creates `main_task` on core 0. It runs every 10 ms and:

1. Reads the newest ESP-NOW control packet.
2. Falls back to the previous packet when no fresh packet is available.
3. Forwards the button mask to the directional-light module.
4. Forwards the complete packet to the proximity/RGB module.
5. Handles knock-triggered hazard clearing.
6. Updates the status LED and sends periodic telemetry.

### UART and audio task

`uart_input_task` runs independently and converts UART characters into sound commands. `SD_init()` mounts the microSD card, initializes I2S, allocates DMA-capable buffers, and starts `audio_task`.

The audio task owns the WAV file and I2S stream. Only one pending command is retained. A new command requests cancellation of the current stream and replaces the pending command, preventing a long music file from blocking control input.

### ESP-NOW receiver

`esp_now_receiver.c` registers the sender as an unencrypted ESP-NOW peer. The receive callback performs a length and MAC check, copies valid data under a mutex, and records the last receive tick. Telemetry and link-recovery heartbeats are serialized through a send mutex. A low-priority recovery task retries peer registration and heartbeat transmission for a bounded seven-second window after link loss.

### Directional lights

`directions_inters.c` receives button masks through a FreeRTOS queue and runs its output logic on core 1. Knock-activated hazards have priority over normal D-PAD commands until cleared by the A button.

### Proximity and RGB

`proximity_RGB.c` reads GPIO34 and GPIO35 through ADC1, selects the higher sensor value, applies filtering and a deadband, and drives a WS2812 strip on GPIO5. Its processing task runs on core 1.

### Servo module

`servo_sweep.c` generates synchronized 50 Hz servo signals on GPIO16 and GPIO17 and owns its servo task.

## Audio Data Flow

The audio path is intentionally constrained to PCM WAV files with 16-bit samples:

1. `read_wav_header()` scans RIFF chunks until it finds `fmt ` and `data`.
2. The parser validates PCM format, 16-bit samples, one or two channels, `block_align`, and `byte_rate`.
3. The sample rate is applied to the I2S clock while the channel is disabled.
4. SD data is read in frame-aligned blocks with bounded retries.
5. Mono samples are duplicated; stereo samples remain left/right interleaved.
6. PCM samples are written to the PCM5102A through Philips-format stereo I2S.
7. DMA transfers are bounded and frame-aligned. Silence is sent before the channel is disabled at end of file.

The SDMMC bus uses four data lines at 40 MHz. The current mount configuration is intended for FAT32 media with 512-byte sectors and a 32 KB allocation unit when formatting is requested. External pull-ups are expected on the SD lines.

## Communication and Safety

The packed protocol is shared with the controller through `common/shared_protocol.h`. Any field or ordering change must be made on both sender and receiver. The receiver tracks link freshness through the last valid control packet and provides a recovery path when packets stop arriving. Vehicle-specific actuator failsafe behavior should remain in the relevant output module rather than inside the ESP-NOW callback.

## Important Boundaries

- Keep callbacks short and defer work to tasks or queues.
- Do not access the shared ESP-NOW packet without the receiver mutex.
- Do not read the SD card from multiple tasks concurrently.
- Keep PCM buffers in internal DMA-capable memory.
- Do not change packed protocol structures without updating both projects.
