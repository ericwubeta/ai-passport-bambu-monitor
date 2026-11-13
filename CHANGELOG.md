# Changelog

All notable changes to this project are documented here.

## v1.5.1 — 2026-11-13 (PrintPet Core)

- Remove the printer-control page, selection/stop-confirmation state, pause/resume/stop APIs, chamber-light API and generic command publisher.
- Keep progress, temperatures, AMS, HMS/device data, multi-printer navigation and fixed `pushall` status refresh. OK on the progress page still toggles remaining time/ETA; other detail pages request status only.
- Use PrintPet Core on the boot, menu and provisioning screens, keeping FoloToy AI Passport as the third-party hardware name. Read the displayed version from the ESP-IDF `PROJECT_VER` build property.
- Add native tests that capture MQTT output and exercise the actual LVGL monitoring navigation with synthetic 1–4-printer and 0–4-AMS fixtures.

The original v1.5.0 tag is unchanged. The release includes an application-only image at 0x10000 and upgrade instructions. Build and host tests passed; physical acceptance of this binary remains pending. Spirit remains unpublished.

## v1.5.0 — 2026-10-31

### Added

- **Idle screen-off** — after 30 s without a key press the backlight turns off and LVGL stops its 5 ms tick, so the badge is dark instead of glowing all day. Any key wakes it and lights the screen again.
- **Power management** — Wi-Fi modem sleep, audio that opens the codec only while a prompt tone is playing, and automatic light sleep with 80/160 MHz frequency scaling once the screen is off. Measured on a 500 mAh pack: average current dropped from about 79 mA to about 30 mA (idle runtime roughly 6 h → 15 h).
- **USB guard** — automatic light sleep is held off whenever the USB-serial console is attached, because the native USB port is unavailable in light sleep on this chip; this also keeps flashing and serial logs working.

### Fixed

- **Stuck on "Connecting" after the screen woke up** — the Bambu cloud answers the first state request with a single ~11.7 kB encrypted record, which needs one contiguous 11.7 kB block. With the previous memory layout that allocation failed, the TLS read aborted and the client looped in reconnect without ever showing printer data. Memory is now laid out so the record fits: LVGL pool 64 → 56 KB (48 KB makes LVGL write out of bounds), Wi-Fi dynamic buffers 32 → 16 and static RX 10 → 8, and TLS certificate copies are released after the handshake.

## v1.4.0 — 2026-10-24

### Added

- **Phone-only first-time setup** — the provisioning page now joins Wi-Fi immediately (APSTA keeps the hotspot alive) and offers Bambu SMS login: phone number → SMS code → the device calls `sendsmscode` / `login` / `user/bind` itself, then fills User ID, accessToken and up to 4 printer serials, saves to NVS and reboots. No PC script or manual token entry is required.
- **Live network status** — the page polls `/netstatus` and shows the connected SSID and IP before login is allowed.
- **Advanced fallback section** — manual User ID / token / serial entry stays available in a collapsed section; the PC `get-token.bat` / `weblogin.py` tools remain as a backup.

### Changed

- The device now waits for SNTP time sync before the HTTPS login, because TLS certificate validation fails when the clock is still at epoch 0.
- Provisioning HTTP task stack raised to 12 KB for the TLS handshake; Wi-Fi auto-reconnect is briefly paused while the page switches networks.

## v1.3.0 — 2026-10-21

### Added

- **Multi-network Wi-Fi (up to 3)** — networks are kept in NVS; on boot the badge connects to the strongest known network and rotates to the next one after 3 failed attempts, so it moves between home Wi-Fi and a phone hotspot without re-provisioning. Legacy single-network settings migrate into slot 0 automatically.
- **Scan-and-pick provisioning** — the provisioning page scans nearby 2.4 GHz access points; tap one to fill the SSID, and add / overwrite / delete saved networks from the phone.
- **On-device Wi-Fi switcher** — new "Wi-Fi" entry in the menu (long-press OK) lists saved networks, marks the currently connected one and switches on OK with progress / result feedback.
- **Current Wi-Fi on screen** — the status bar shows `Cloud online · <SSID>`; the device page shows the SSID and signal strength on separate rows.

### Fixed

- Entering provisioning from the menu rebooted the device: switching to APSTA while the STA interface was running made the Wi-Fi driver start softAP with an uninitialized configuration (crash in `ieee80211_hostap_attach`). Wi-Fi is now stopped, configured and restarted.
- Up/down keys did not move the menu selection, so the provisioning entry could never be reached.
- The home screen could show AMS content: pages were created visible with a transparent background and drew over the progress page. Pages are now created hidden and visibility is applied centrally.
- The LVGL pool setting was silently ignored (`LV_MEM_SIZE_KILOBYTES` is deprecated in LVGL 9); the intended 64 KB pool is now applied via `LV_MEM_SIZE`.
- A failed allocation could halt the LVGL task forever (`LV_USE_ASSERT_MALLOC`); the assert is now disabled so rendering degrades gracefully instead.
- Device-page Wi-Fi text overlapped the battery row; SSID and signal strength are now separate lines.

### Changed

- Removed the battery-millivolt row from the device page (the header already shows battery percentage and icon).
- Provisioning page responses are sent with `Cache-Control: no-store` so phones do not keep a stale form.
