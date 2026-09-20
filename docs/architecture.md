# Architecture

Tab5 OS is an ESP-IDF/FreeRTOS firmware with one LVGL display shell. It deliberately uses the platform's native drivers, NVS, SPIFFS, FAT VFS, HTTP/TLS, OTA, NimBLE, and ordinary files instead of introducing an application framework or database.

## Boot and runtime

`app_main` runs pure assert-based parser/math checks, initializes the shared internal I2C devices, display/touch, ESP-Hosted Wi-Fi transport, NVS-backed settings, internal SPIFFS, and microSD, then creates the fixed header/content shell. Reaching the `Starting Tab5 OS` boot log means the pure checks passed. A 30-second LVGL timer validates an OTA candidate only after NVS and internal storage are usable and the event loop remains alive.

LVGL objects and callbacks run on the display task under the BSP display lock. Network, audio, ADC, BLE, download, and cleanup work runs in bounded FreeRTOS tasks or timers. Workers never update deleted LVGL objects directly: they store bounded state/results, and the visible app's LVGL timer renders them.

## Launcher and lifecycle

The static launcher table in `main/main.c` owns each tile's enter callback and optional leave hook. `clear_content()` invokes the active leave hook, stops shared tools/timers, releases pins and outputs, nulls screen pointers, deletes content children, and resets scroll/layout state. New substantial tools live in their own `main/*_tool.c` file; legacy code moves only when materially changed.

Every resource has one owner at a time:

- G53/G54: GPIO, Scope ADC, Servo Toy, or external I2C.
- G6 and LEDC timer 2/channel 3: Signal Generator.
- LEDC timer 1/channel 2: Servo; display backlight remains timer 0/channel 1.
- UART1: G47/G48 terminal or onboard RS-485 through its explicit mode.
- SPI2: bounded M5-Bus transaction console.
- BLE connection/scanner: optional product view or generic GATT Explorer, never both.
- SD writer/capture: the owning app until stop/commit/abort completes.

Leaving an app restores shared pins to disabled/high-impedance or an explicitly safe receive/off state. OTA checks the same ownership state and refuses restart while a writer, output, sampling task, network operation, or BLE session is active.

## Storage

- NVS stores small validated settings and credentials. Initialization faults preserve data rather than auto-erasing it.
- Internal `storage` is SPIFFS for built-in state. Only provably blank flash auto-initializes.
- FAT microSD stores user files and evidence. Writers use durable sync, unique `.TMP` publication, recoverable `.BAK` replacement, or incomplete-tail repair according to the data type.

See [Storage and compatibility contract](compatibility.md) and [Data formats](data-formats.md).

## Network and radio

The ESP32-P4 uses the onboard ESP32-C6 over the Tab5 fixed four-bit SDIO bus for Wi-Fi and BLE. Offline startup is valid; wired tools and the launcher do not depend on cloud availability. HTTP, MQTT, browser, weather, relay, and OTA traffic use bounded buffers and timeouts. TLS certificate bundles verify secure transports. Plain HTTP/MQTT requires an unchanged second tap within five seconds.

Chat and transcription send authenticated HTTPS requests to a separately deployed relay, which holds the upstream OpenAI API key. The tablet stores only a revocable device token. Generic BLE scanning and connection are explicit; product profiles are off at boot.

## Build and release

`sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`, and `dependencies.lock` are release inputs. Compile-time guards reject the wrong ESP-Hosted SDIO pins or a hardware-UART console. Push/PR CI runs host checks and a clean ESP-IDF build. Stable tags additionally create app-only and 16 MiB factory images, an OTA manifest, checksums, and license/dependency notices. See [OTA manifest contract](ota-manifest.md).
