# Build & Flash (ESP-IDF)

Prerequisites
- Install ESP-IDF (version compatible with the project's `esp-idf` directory). Follow Espressif's official setup for Windows.
- Target hardware: classic ESP32 DevKit V1 (`esp32` target), not ESP32-S2/S3/C3.
- Ensure `IDF_PATH` and toolchain are configured, or use the ESP-IDF PowerShell environment provided by the tools.
- In VS Code, confirm the ESP-IDF extension uses `C:\esp\v5.5.4\esp-idf` for this project. If CMake reports `/tools/cmake/project.cmake` or `IDF_PATH` is empty, reload the ESP-IDF extension or reopen the ESP-IDF terminal before building.

Quick commands (from repository root `RC_Car_Receiver`)
1. Configure (optional):
```
cd RC_Car_Receiver
idf.py menuconfig
```
2. Build:
```
idf.py build
```
3. Flash to device (auto-detect or specify serial port):
```
idf.py -p COM4 flash monitor
```
Adjust `-p` if the receiver appears on a different serial port. Run this command from the `RC_Car_Receiver` directory; it flashes the classic ESP32 image with `--chip esp32`.

Do not flash the `Xbox_USB_HID_Communication` image to this board. That project targets the separate ESP32-S3 USB-host board and its command uses `--chip esp32s3`.

The receiver uses ADC1 channels 6 and 7, which map to GPIO34 and GPIO35 on the classic ESP32 DevKit V1.

Notes about build artifacts
- Build output goes to `RC_Car_Receiver/build/`. The repository stores an example `sdkconfig` and generated build config in `build/config/`.

Common issues
- `Configuring incomplete` with `include could not find requested file: /tools/cmake/project.cmake`: `IDF_PATH` was not inherited by the build terminal. Set it to `C:\esp\v5.5.4\esp-idf` through the ESP-IDF extension setup, then reconfigure.
- `This chip is ESP32, not ESP32-S3`: the ESP32-S3 sender image is being flashed to the receiver. Change to the `RC_Car_Receiver` project directory and run the receiver flash command above.
- Missing toolchain: install the Xtensa/RISC-V toolchain matching the target CPU.
- Wrong IDF version: the project vendor files under `build/esp-idf/` reflect the embedded IDF version; use that IDF release where possible.

Keeping configuration in sync
- `sdkconfig` in the project root is the base configuration. `idf.py menuconfig` will write to the local `sdkconfig` and `build` config files.
