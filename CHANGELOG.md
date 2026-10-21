# Changelog

All notable changes to this project are documented here.

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
