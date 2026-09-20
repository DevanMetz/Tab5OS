# Third-party notices

Tab5 OS is licensed under Apache License 2.0. The release `LICENSE` file contains that license text. The firmware also links the following pinned projects; exact registry hashes and transitive relationships are recorded in the released `dependencies.lock`.

| Project | Version | License |
| --- | ---: | --- |
| ESP-IDF | 5.4.2 | Apache-2.0, with bundled third-party components identified by ESP-IDF's SPDX/SBOM metadata |
| LVGL | 9.5.0 | MIT; see `LVGL_LICENSE.txt` and `LVGL_COPYRIGHTS.md` in the release |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 |
| espressif/eppp_link | 1.1.5 | Apache-2.0 |
| espressif/esp_codec_dev | 1.3.4 | Apache-2.0 |
| espressif/esp_hosted | 1.4.0 | Apache-2.0 |
| espressif/esp_lcd_ili9881c | 1.0.1 | Apache-2.0 |
| espressif/esp_lcd_st7703 | 1.0.3 | Apache-2.0 |
| espressif/esp_lcd_touch | 1.1.2 | Apache-2.0 |
| espressif/esp_lcd_touch_gt911 | 1.1.3 | Apache-2.0 |
| espressif/esp_lcd_touch_st7123 | 1.0.0 | Apache-2.0 |
| espressif/esp_lvgl_port | 2.5.0 | Apache-2.0 |
| espressif/esp_serial_slave_link | 1.1.2 | Apache-2.0 |
| espressif/esp_wifi_remote | 0.8.5 | Apache-2.0 |
| espressif/mdns | 1.11.3 | Apache-2.0 |
| espressif/usb_host_hid | 1.0.3 | Apache-2.0 |
| M5Stack Tab5 BSP-derived components | repository revision | Apache-2.0 |
| Espressif ST7121 panel driver | repository revision | Apache-2.0 |

LVGL's own copyright inventory identifies optional and bundled libraries under additional permissive licenses. The release includes that upstream inventory unchanged. Only libraries enabled by the compiled configuration are linked into the firmware.

Source and license locations:

- ESP-IDF: <https://github.com/espressif/esp-idf>
- Espressif Component Registry: <https://components.espressif.com/>
- LVGL: <https://github.com/lvgl/lvgl>
- M5Stack Tab5 user demo/BSP source: <https://github.com/m5stack/M5Tab5-UserDemo>

This notice is informational and does not replace any license text shipped with the corresponding source or release.
