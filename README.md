# Tab5 OS

A small, open operating environment for the M5Stack Tab5. It currently boots an LVGL home screen and verifies the display, touch controller, and ESP32-P4 runtime.

## Hardware

- M5Stack Tab5 (ESP32-P4)
- Original ILI9881C/GT911 and newer ST7123/ST7121 display variants through M5Stack's factory BSP

## Toolchain

M5Stack recommends ESP-IDF v5.4.2 for Tab5. Install that exact release using Espressif's setup, then open an ESP-IDF shell.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

Hold the Tab5 reset button until its green LED flashes rapidly before flashing. Use `Ctrl+]` to exit the monitor.

The checked-in defaults include M5Stack's required QIO, 200 MHz PSRAM, and L2-cache settings. Removing them causes MIPI display underruns on the 720p panel.

## Upstream

The small board-support components in `components/` come from M5Stack's Apache-2.0-licensed [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo). Managed dependencies are pinned in `dependencies.lock`.

## Status

- [x] ESP32-P4 boot
- [x] Display and backlight
- [x] Touch input
- [ ] Storage and file browser
- [ ] Wi-Fi settings
- [ ] Application launcher

## License

Apache-2.0
