# Core host checks

These checks use dummy printer/account data and never connect to Bambu Cloud or a printer.

`test_mqtt_readonly` compiles the actual `main/app_mqtt.c` byte-for-byte with transport, timer and configuration fixtures. It parses every outgoing JSON payload and requires only `pushing.command = pushall`, the expected request topic, QoS 0 and retain 0. It exercises initial connection, manual refresh, invalid indices, timer polling and reconnection for 1–4 printers, and checks incoming telemetry still updates the intended snapshot.

`test_ui_readonly` compiles the actual Core page implementation and LVGL. It covers the no-report page set, 0–4 AMS, forward/backward wrapping, progress ETA toggling, camera activation and refresh, persisted camera and legacy page slots, detail-page refresh and 2–4-printer overview/detail reentry. No control label/object may be constructed. Generated PPM captures contain only synthetic fixtures.

## Run

Use the existing native C/C++ compiler and CMake. `CJSON_DIR` points to the installed ESP-IDF source tree; the optional UI check uses the existing LVGL dependency from `managed_components/lvgl__lvgl` or an explicit `LVGL_DIR`.

```powershell
cmake -S tests/host -B build-host -DCJSON_DIR=E:/esp-idf/components/json/cJSON -DPRINTPET_UI_TESTS=ON
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure
```

For only the transport check, omit `PRINTPET_UI_TESTS`. On another platform, substitute that platform's ESP-IDF path and CMake generator.

The host MQTT translation unit is copied unchanged into the build directory before compilation so that the optional private header resolves to the dummy test header, never a developer's private `main/secrets.h`. Test-target compiler options keep assertions enabled in every build configuration.

The LVGL host pool is 128 KiB to accommodate native object/pointer sizes. It tests page behavior and rendering, not ESP32-C3 RAM capacity. Real simultaneous printers, board buttons, audio, sleep/wake and long-run device behavior still require on-device acceptance. Building/testing does not flash hardware.
