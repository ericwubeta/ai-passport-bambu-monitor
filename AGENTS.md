# PrintPet firmware maintenance

Preserve historical tags and all unrelated local changes. Core1.5.2 and Spirit2.0.1 are separate product choices; neither controls printers. Build and host checks are separate from physical device acceptance.

This source uses a shared ESP32-C3 8MiB layout: app0x370000/4MiB, resources0x10000/3MiB, read-only cardid0x356000/0x4000. Never erase or write the vendor gap0x310000–0x36FFFF or copy another device identity. Only complete sparse packages may be offered for installation/recovery; follow docs/INSTALL.md. Building/publication does not authorize device access or flashing.

Never commit secrets.h, credentials, personal NVS/Flash backups or raw device logs. Use placeholder secrets for release builds. Reuse BSP drivers, preserve LVGL locking and nonblocking button callbacks, and verify Chinese glyph coverage. Keep software/toolchains/build outputs on E: on the original workstation.

Keep hardware-demo UI in English. Keep the printer-monitor labels in Chinese, including on the camera page. Camera connection and configuration prompts may be in English. Run the read-only host tests in tests/host and the camera and screen regressions in tools/tests when changing shared monitoring behavior.
