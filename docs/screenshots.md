# Screenshot sources

The [README gallery](../README.md#screenshots) shows the actual development UI from 2026-09-29. The selected screens use built-in or synthetic examples and contain no credentials, SSIDs, IP/MAC addresses, private documents or nearby device identities. No pixel redaction was needed for these selected captures; private network/file screens remain excluded.

| Image | Source | Example shown |
| --- | --- | --- |
| [Launcher](images/launcher.png) | ST7121 tablet framebuffer over USB | App tiles; no app opened or private contents shown. |
| [Byte Lab](images/byte-lab.png) | Native LVGL host render from `tests/offline_ui_test.c` | Float32 `1.5`, little endian, bytes `00 00 C0 3F`. |
| [RTU Frames](images/rtu-frames.png) | ST7121 tablet framebuffer over USB | Built-in function 03 example, two registers decoded as Float32 `1.5`; no device contacted. |

The PNGs are unchanged copies of the reviewed captures. Hardware captures originally live in ignored `build/hardware-20260929/` as `launcher-fixed.png` and `rtu-example.png`; the host render is `build/offline-ui/bytes-encode-float32.png`. Only the three public example images are copied into `docs/images/`.

## Reproduce the host example

After building firmware once to fetch the pinned LVGL sources, install a native C compiler, CMake and Ninja on PATH and run from the repository root:

```powershell
.\tools\test_offline_ui.ps1 -Snapshots
```

This renders the actual LVGL widgets with synthetic inputs and writes PPM previews under `build/offline-ui/`, including `bytes-encode-float32.ppm`. Convert that preview to PNG for publication. See [Testing](testing.md#offline-and-peripheral-ui) for setup and coverage.

## Adding a screenshot

Use built-in examples or synthetic fixture data. Review the entire visible frame, including the header, status text, payloads and clipboard, before sharing. For a screen that needs real data, replace sensitive values before capture or apply opaque redaction and inspect the exported image. Never commit the raw private capture.

Record the date, firmware or host source, and whether the image came from a physical tablet or a host render. A screenshot illustrates the UI; measured hardware coverage remains in the [smoke checklist](hardware-smoke-checklist.md).
