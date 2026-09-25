# Development Guide

## Start Here

1. Follow [BUILD_FLASH.md](BUILD_FLASH.md) to configure, build, flash, and monitor the project.
2. Review the packet definitions in [common/shared_protocol.h](../../common/shared_protocol.h).
3. Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing task ownership or shared state.

Important source files:

- [main/main.c](../main/main.c): application startup, the 100 Hz control loop, UART commands, telemetry cadence, and knock handling.
- [main/esp_now_receiver.c](../main/esp_now_receiver.c): ESP-NOW initialization, receive callback, packet storage, telemetry, and link recovery.
- [main/directions_inters.c](../main/directions_inters.c): queued turn-signal and hazard behavior.
- [main/proximity_RGB.c](../main/proximity_RGB.c): ADC filtering and WS2812 output.
- [main/servo_sweep.c](../main/servo_sweep.c): servo PWM and sweep task.
- [main/SD_sdmmc.c](../main/SD_sdmmc.c): SDMMC mounting, WAV parsing, I2S configuration, DMA streaming, and audio recovery.
- [main/SD_sdmmc.h](../main/SD_sdmmc.h): public audio initialization and playback API.

## Build and Monitor

Use the ESP-IDF extension or the ESP-IDF terminal environment. The normal command sequence is:

```text
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

Use `idf.py menuconfig` for project configuration. The receiver's current audio path expects FAT32 media with 512-byte sectors, 16-bit PCM WAV files, and external SDMMC pull-ups.

## Task and Concurrency Rules

- Keep the 100 Hz `main_task` responsive. Do not perform SD access, long delays, or blocking audio operations in it.
- Keep ESP-NOW callbacks short. Copy data and signal a task; do not parse files or drive slow peripherals from a callback.
- Protect shared ESP-NOW state with the existing receiver mutex.
- Do not access the mounted SD card concurrently from multiple tasks.
- Keep audio input and output buffers in internal memory with `MALLOC_CAP_DMA`.
- Use queues for cross-task commands. The audio queue intentionally retains only the most recent pending command.
- Keep ISR handlers minimal and defer debounce or application logic to a task.

## ESP-NOW Changes

The sender and receiver share packed structures. When changing the protocol:

1. Update [common/shared_protocol.h](../../common/shared_protocol.h).
2. Update the sender producer and receiver consumer together.
3. Keep field order and widths identical on both devices.
4. Rebuild both projects before flashing either device.
5. Test normal packets, link loss, peer recovery, and telemetry responses.

The receiver filters packets by length and sender MAC address. Telemetry and recovery heartbeats share a send mutex to avoid overlapping ESP-NOW transmissions.

## Audio Changes

The supported audio contract is uncompressed PCM WAV, 16-bit, mono or stereo. The parser validates the RIFF header, `fmt ` chunk, `data` chunk, channel count, `block_align`, and `byte_rate`.

When changing audio code:

1. Preserve left/right interleaving for stereo files.
2. Duplicate mono samples into both I2S slots.
3. Keep SD reads aligned to complete PCM frames.
4. Keep I2S writes aligned to the 4-byte stereo frame size.
5. Preserve bounded timeouts and retries for both SD and I2S.
6. Test short effects, mono files, stereo files, and long music files.

Audio commands are:

```text
1 sndaL.wav       5 music_A.wav
2 sndaR.wav       6 music_B.wav
3 sndb.wav        7 music_C.wav
4 sndc.wav
t toggle console telemetry
h or ? print help
```

A new sound request cancels the current stream and replaces the pending request. Do not turn this queue into an unbounded queue; long files must not delay control behavior.

## Hardware and SDMMC Notes

- SDMMC uses GPIO14/15/2/4/12/13 for CLK/CMD/D0/D1/D2/D3.
- The bus is 4-bit and currently configured for 40 MHz with a 2-second command timeout.
- Use external 10 kOhm pull-ups on the SD lines; the firmware disables the internal slot pull-up flag.
- PCM5102A uses GPIO23/GPIO21/GPIO22 for BCLK/LRCK/DIN. MCLK/SCK is unused.
- If SDMMC initialization fails with `0x107`, first inspect 3.3 V power, grounding, pull-ups, wiring length, and card compatibility before changing filesystem settings.

## Testing Checklist

- Build both sender and receiver.
- Verify the receiver mounts the intended card and prints the expected card type and capacity.
- Play every WAV command, including short effects and long music files.
- Verify mono is audible in both channels and stereo preserves channel separation.
- Disconnect the sender and confirm control-link recovery without blocking audio or the main loop.
- Trigger the knock sensor and verify hazard activation, telemetry rumble, debounce, and A-button clearing.
- Inspect `ESP_LOGW` messages for SD retry, I2S DMA, or ESP-NOW recovery events.
