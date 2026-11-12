# Repository Guidelines

## Project Structure & Module Organization

This repository is PrintPet firmware for the third-party FoloToy AI Passport hardware (ESP32-C3, no PSRAM), a community derivative without FoloToy or Bambu Lab endorsement.

## Product Names and Publication State

- PrintPet Core is the 1.x monitoring line. The current 1.5.1 source removes the control UI and actuator APIs and only emits fixed pushall telemetry requests. The historical v1.5.0 tag still contains the old paths and must not be rewritten.
- PrintPet Spirit is the 2.x line with character animation and voice. Spirit 2.1.8 remains in internal testing; its source, assets and binaries are not published here. Naming maintenance does not authorize publishing them or creating tags/releases.
- The Windows product remains PrintPet - 打印小监工. Sharing a brand does not imply firmware AI chat or cross-device synchronization.
- AI Passport refers only to FoloToy's third-party hardware. Do not use it as the firmware product name or imply official affiliation.
- Canonical firmware repository: ericwubeta/printpet-firmware. Preserve passport_printer build/artifact names and existing configuration identifiers for compatibility.

## Project Structure

- `components/bsp/include/`: public BSP APIs and the hardware pin/configuration source of truth (`bsp_pins.h`).
- `components/bsp/src/`: display, button, audio, battery, and shared-I2C implementations.
- `main/`: firmware startup, printer telemetry, provisioning and LVGL pages. `demo.h` retains the menu lifecycle interface.
- `sdkconfig.defaults`: reproducible target, console, LVGL, and memory defaults.
- `README.md` / `README.zh_CN.md`: product lines, historical behavior differences, hardware and validation limits.

Keep reusable hardware logic in `components/bsp`; keep board demonstration and UI behavior in `main`.

## Build, Test, and Development Commands

Use ESP-IDF 5.5.x:

```bash
get_idf553                    # Enter the repository's ESP-IDF 5.5.3 environment
idf.py set-target esp32c3     # Configure a fresh checkout
idf.py build                  # Compile firmware and validate dependencies
idf.py flash monitor          # Flash the connected board and open logs
idf.py fullclean              # Remove generated build state when configuration is stale
```

Treat a clean `idf.py build` as the minimum check. `tests/host/README.md` documents native MQTT transport and actual LVGL navigation tests with synthetic fixtures. Host tests do not establish physical multi-printer behavior or ESP32-C3 RAM margins. Device flashing and on-device acceptance require the applicable task authorization.

## Coding Style & Naming Conventions

Write C using four-space indentation and K&R-style braces, following nearby files. Use `snake_case` for functions and locals, `BSP_*` for public hardware constants, and `s_` for file-local state. Keep BSP APIs prefixed with `bsp_`; name demo entry points `demo_<feature>_<action>`. Prefer `static` for internal symbols. UI text stays English; explanatory comments may be Chinese. Preserve comments documenting hardware-specific register values and memory constraints.

## Testing Guidelines

Before submitting, build from the repository root and inspect warnings. On hardware, verify menu navigation and the affected Display, Button, Audio, or Battery page. For pin, display-rotation, codec-clock, ADC, or DMA changes, explicitly record the observed hardware result in the PR. Do not increase LVGL buffers or audio allocations without checking ESP32-C3 internal RAM usage; the board has no PSRAM.

## Commit & Pull Request Guidelines

History follows Conventional Commit-style subjects such as `feat(bsp): ...`, `feat(demo): ...`, `fix(bsp): ...`, and `docs: ...`. Keep commits focused by subsystem. Pull requests should explain the hardware/revision tested, summarize behavior changes, list build and on-device results, and include photos or screenshots for display changes. Link related issues and call out wiring, pin-map, or compatibility impacts.
