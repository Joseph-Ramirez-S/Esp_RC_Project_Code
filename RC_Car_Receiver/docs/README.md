# RC_Car_Receiver

ESP-IDF firmware for an ESP32-based RC car receiver. The receiver accepts controller state over ESP-NOW and drives the car's lighting, proximity visualization, servo outputs, and PCM5102A audio output.

## Documentation

- [Architecture and runtime flow](ARCHITECTURE.md)
- [Packet protocol](PROTOCOL.md)
- [Build and flash instructions](BUILD_FLASH.md)
- [Development guide and code map](DEVELOPMENT_GUIDE.md)

## Runtime Summary

- The main control task runs at 100 Hz and retains the last valid controller packet when no new packet is available in a cycle.
- ESP-NOW control packets are received from the configured sender MAC address. Telemetry is sent approximately once per second and immediately for knock-triggered rumble events.
- Directional lights are processed by a queue-driven task on core 1.
- Proximity inputs are sampled on GPIO34 and GPIO35. The higher reading drives a smoothed WS2812 visualization on GPIO5.
- The KY-031 knock sensor activates hazard behavior and can trigger an immediate telemetry rumble flag.
- UART sound commands are handled by a separate task so audio playback does not block the 100 Hz control loop.

## Hardware

| Function | GPIO or interface |
| --- | --- |
| Status LED | GPIO27 |
| Left turn signal | GPIO32 |
| Right turn signal | GPIO33 |
| Proximity input 1 | GPIO34, ADC1 channel 6 |
| Proximity input 2 | GPIO35, ADC1 channel 7 |
| WS2812 data | GPIO5 |
| Knock sensor | GPIO36, rising-edge interrupt |
| SDMMC CLK | GPIO14 |
| SDMMC CMD | GPIO15 |
| SDMMC D0 | GPIO2 |
| SDMMC D1 | GPIO4 |
| SDMMC D2 | GPIO12 |
| SDMMC D3 | GPIO13 |
| PCM5102A BCLK | GPIO23 |
| PCM5102A LRCK/WS | GPIO21 |
| PCM5102A DIN | GPIO22 |

The SD card uses 3.3 V logic, a 4-bit SDMMC bus, and external 10 kOhm pull-ups. The current firmware configures the SDMMC clock at 40 MHz and uses a 2-second command timeout. The PCM5102A runs as an I2S Philips-format stereo sink. MCLK/SCK is left unused.

## Audio Files

Place these files in the root of the FAT32 microSD card:

| Command | File |
| --- | --- |
| `1` | `sndaL.wav` |
| `2` | `sndaR.wav` |
| `3` | `sndb.wav` |
| `4` | `sndc.wav` |
| `5` | `music_A.wav` |
| `6` | `music_B.wav` |
| `7` | `music_C.wav` |

Files must be uncompressed PCM WAV with 16-bit samples and one or two channels. Mono input is duplicated to left and right output. The sample rate is read from the WAV header and applied to the I2S clock.

The audio path uses DMA-capable internal buffers, bounded I2S write timeouts, frame-aligned transfers, cancellation of the current sound when a new command arrives, and bounded SD-card recovery retries.

## UART Commands

- `1` through `7`: queue the corresponding sound. A new command replaces any pending command and requests cancellation of the current sound.
- `t`: toggle the live console telemetry line.
- `h` or `?`: print command help.
