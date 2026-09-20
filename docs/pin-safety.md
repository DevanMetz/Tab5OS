# Pin and interface safety

Tab5 external logic is 3.3 V only. Do not connect 5 V pull-ups or logic directly to a GPIO. External 5 V stays off unless a future control explicitly enables it.

## Ownership

| Interface | Pins | Tab5 OS policy |
| --- | --- | --- |
| USB console, flashing, and remote desktop | USB Type-C Serial/JTAG | Always reserved. Firmware builds require this to be the sole console. |
| Grove I2C, GPIO, and Servo Toy | G53/G54 | One tool at a time. Leaving the tool releases both pins; I2C requires 3.3 V pull-ups. |
| ADC Scope | G16/G18/G19/G49/G50/G51/G53/G54 | Input-only, 0-3.3 V maximum. Per-input scale/offset are software corrections, not input protection or proof of calibration. G18/G19 also belong to SPI and G53/G54 to Grove tools. |
| UART terminal | UART1, G47 TX/G48 RX | Default 3.3 V M5-Bus pair. Settings changes require the terminal to be stopped; both pins are released on exit. |
| Legacy M5-Bus UART0 labels | G37 TX/G38 RX | Not the default: both are ESP32-P4 strapping pins and connected equipment can disturb reset. |
| Onboard RS-485 | G20 TX/G21 RX/G34 DIR | SIT3088 direction is automatic. The 120-ohm terminator is the physical switch; enable it only at a bus end. Stop leaves DIR low and releases the GPIOs. |
| SPI master | SPI2: G18 MOSI/G19 MISO/G5 SCK/G45 CS | 3.3 V M5-Bus signals. The tool starts stopped, caps each transfer at 32 bytes, and releases all four pins on Stop/Home. G18/G19 are also Scope inputs, so use one tool at a time. |
| PWM and pulse generator | G6 | 3.3 V logic signal only, never a power output. LEDC timer 2/channel 3 do not share the backlight or Servo resources. PWM stops after five minutes; Stop/Home and each single pulse release G6. |

The console decision intentionally leaves every hardware UART available to tools while preserving USB flashing, boot logs, and recovery. The UART terminal uses UART1 on G47/G48; UART0-labelled G37/G38 remain an advanced compatibility choice, not a safe default.

The RS-485 connector is J7: pin 1 is GND, pin 2 is system VIN, pin 3 is A, and pin 4 is B. Use pins 1/3/4 for communications and leave VIN disconnected while Tab5 is USB-powered. Pins 5/6 are the internal system I2C lines, not RS-485 signals. Never connect termination at every node; use the physical 120-ohm switch only when Tab5 is at an end of the bus.

The SPI console uses G45 as its explicit active-low chip-select. Confirm the target's voltage, SPI mode, maximum clock, and power sequencing before pressing Start. Tab5 OS does not power an external target or infer its protocol.

The signal generator cannot directly drive a motor, relay, solenoid, high-power LED, speaker, or 5 V logic. Use a suitable buffer/driver and protection, share ground, and verify the requested waveform with an oscilloscope or logic analyzer before connecting a sensitive target. Pulse widths are requested software timing until physically characterized.

Scope frequency and duty use threshold crossings in the sampled ADC window, so accuracy is limited by the selected sample rate, trigger level, noise, and analog bandwidth. Calibrate each input against known low and high references; the offset and scale controls cannot make an over-voltage safe.

Sources: [M5Stack Tab5 pin map](https://docs.m5stack.com/en/core/Tab5), [ESP-IDF USB Serial/JTAG console](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32p4/api-guides/usb-serial-jtag-console.html), and [ESP32-P4 GPIO/strapping pins](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32p4/api-reference/peripherals/gpio.html).
