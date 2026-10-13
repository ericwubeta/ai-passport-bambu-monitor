# AI Passport · Bambu Lab Printer Monitor

[简体中文](README.zh_CN.md) | English

A wearable Bambu Lab 3D-printer monitor built on the [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) hardware (ESP32-C3).
It subscribes to the Bambu Cloud MQTT stream and shows live print status, temperatures, AMS filament slots and device info on the 240×320 round-corner display — no phone needed.

## Features

- **Live print status** — state, progress %, layer/nozzle counts, elapsed/remaining time
- **Temperatures** — bed, nozzle and chamber target/current values
- **AMS filament** — up to 4 AMS × 4 slots: material, color, name and remaining grams (manual spools without RFID are handled correctly)
- **HMS alerts** — error codes shown in the status bar with a notification sound
- **Device page** — printer model, serial, firmware and connection state
- **Control page** — sends pause/resume/stop and chamber-light commands through the cloud channel (whether the printer executes them depends on the account; third-party tokens are typically read-only)
- **On-device provisioning** — softAP + web page; Wi-Fi and Bambu credentials are stored in NVS, never compiled in
- **Multi-printer** — monitor up to 4 printers on the same Bambu account via a card overview with per-printer detail pages (testing status below)
- **Cyber boot animation** — terminal-style boot log drawn entirely with LVGL primitives
- ~10 s telemetry refresh, heap-stable for days of continuous monitoring

## Screenshots

<p align="center">
  <img src="docs/images/hero-progress.jpg" width="400" alt="Print progress page on the wearable badge">
</p>
<p align="center">
  <em>Print progress: state, temperatures, layers, remaining time</em>
</p>

| AMS filament slots | Print control | Boot animation |
| :---: | :---: | :---: |
| <img src="docs/images/page-ams.jpg" width="240"> | <img src="docs/images/page-control.jpg" width="240"> | <img src="docs/images/boot.jpg" width="240"> |

## Hardware

| Item | Spec |
| --- | --- |
| MCU | ESP32-C3 (single-core RISC-V, 400 KB SRAM, no PSRAM) |
| Display | 1.69" 240×320 ST7789 SPI, rounded corners (r = 30 px) |
| Input | 3 ADC buttons (up / down / OK, long-press OK = back) |
| Audio | ES8311 codec + speaker |
| Fuel gauge | CW2017 (500 mAh Li-Poly profile) |
| Flash | 8 MB (4 MB factory app + SPIFFS/data) |

## How it works

```
Bambu Cloud (cn.mqtt.bambulab.com:8883, TLS)
        │  device/<sn>/report  (push, ~10 s)
        ▼
ESP32-C3  ── JSON parse ── LVGL pages ── ST7789
   │  app_mqtt.c      app_printer.c
   └── credentials from NVS (app_config.c), written by the provisioning page
```

Pages are switched with up/down; long-pressing OK returns to the menu. The button
callback only records the event — heavy rendering runs from an LVGL timer, which
avoids stack overflows on the button component's small callback task.

## First-time setup (no rebuild needed)

1. Flash the firmware and power on the device.
2. When unprovisioned, the device opens a Wi-Fi hotspot:
   - SSID: **Bambu-Setup**　Password: **setup123**
3. Connect a phone/laptop to it and open <http://192.168.4.1>.
4. Fill in:
   - Home Wi-Fi SSID / password
   - Bambu **User ID** (`u_` prefix removed; the numeric part)
   - **accessToken** from the Bambu cloud session
   - Printer **serial number** (15 digits, e.g. `00C00AAAA000000`)
5. Save — the device reboots, joins Wi-Fi and enters the monitor page.

Credentials live only in the device's NVS partition. As a legacy fallback,
`main/secrets.h` (git-ignored; copy from `main/secrets.h.example`) can carry
compile-time defaults.

> Token note: the Bambu accessToken is an account session credential and
> expires periodically. Re-provision when the cloud indicator stays
> disconnected. Only flash firmware containing your own token to devices you
> own.

## Multi-printer support & testing status

Up to **4 printers on the same Bambu account** can be monitored at once:

- The provisioning page accepts 2–4 serial numbers with optional custom names (e.g. `Study X1C`)
- With 2+ printers configured, the device opens a **printer overview** (each card shows name, state dot, progress % and remaining time; printers with HMS alarms are highlighted) — press OK to open a printer's detail pages, long-press OK to return to the overview
- With exactly 1 printer the behaviour is identical to the single-printer firmware
- All printers share one cloud connection (single TLS/MQTT session, one report topic per printer)

**Testing status — please read before relying on this feature:**

| Scenario | Status |
| --- | --- |
| Single printer (regression) | ✅ Verified on hardware, long-run stable, watchdog-free |
| Multi-printer UI, navigation, per-printer data routing | ✅ Verified on hardware in **simulation** (1 real printer + 3 injected test serial numbers) |
| Real data while subscribed to 4 topics at once | ✅ Verified on hardware |
| **Two or more *real* printers streaming simultaneously** | ⚠️ **Not tested** — the author only owns one printer. The topic routing is standard MQTT, but this scenario has no hardware confirmation yet |
| Provisioning page with multiple serial numbers | ⚠️ Form parsing code-reviewed, not end-to-end tested on hardware |

For testing without extra hardware, `main/app_mqtt.c` has a simulation switch: set `TEST_EXTRA_MACHINES` to 1/2/3 to inject that many fake printers next to the real one. **It defaults to 0 (off) and must stay 0 in release builds.**

If you run two or more real printers and hit an issue, please open an issue — real multi-printer feedback is very welcome.

## Building

Requires ESP-IDF **5.5.x** (tested with 5.5 release on Windows).

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COM5 flash monitor
```

Partition layout (`partitions.csv`): NVS + PHY + 4 MB factory app on 8 MB flash.
Flashing keeps NVS, so re-flashing does not erase provisioning data.

## Project layout

```
main/
  app_printer.c     monitor UI: status / temps / AMS / device / control pages
  app_mqtt.c        Bambu Cloud MQTT client + JSON telemetry parsing
  app_provision.c   softAP + HTTP provisioning page
  app_config.c      NVS-backed credential store
  app_wifi.c        Wi-Fi connection (NVS credentials, secrets.h fallback)
  app_boot.c        boot animation
  app_notify.c      HMS alert text + sound
  hms_codes.c       HMS code catalog (Chinese)
  font_zh*.c        subset Chinese pixel fonts
components/bsp/     board support: display, buttons, audio, battery, I2C
tools/              font symbol generation, serial capture, syntax check
```

## Security

- No real credentials are committed. `main/secrets.h` is git-ignored; only the
  `.example` placeholder template is tracked.
- Logs containing serial numbers are local-only (`*.log` is git-ignored).
- When distributing your own build, review `sdkconfig` and never publish a
  binary compiled with real credentials.

## Acknowledgements

- [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) — hardware baseline, BSP and LVGL framework (MIT)
- Bambu Lab — printer telemetry consumed through the official cloud MQTT channel

## License

[MIT](LICENSE), Copyright © 2026 FoloToy, Copyright © 2026 EricWU.
This project is a community derivative and is not affiliated with or endorsed by Bambu Lab or FoloToy.
