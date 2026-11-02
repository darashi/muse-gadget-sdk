# M5Stack Tab5 (experimental)

The Tab5 port runs Muse on the ESP32-P4 with a portrait 720×1280 touchscreen,
16 MB flash and 32 MB PSRAM. Its ESP32-C6 supplies Wi-Fi and BLE over SDIO.
The display, touch and audio drivers come from Espressif's
[`m5stack_tab5_noglib` BSP 1.3.2](https://components.espressif.com/components/espressif/m5stack_tab5_noglib/versions/1.3.2).
Muse uses `esp_lvgl_adapter` for LVGL; the BSP's standard `esp_lvgl_port` 2.9.0
references a DSI callback that ESP-IDF 6 removed.

The BSP detects the ILI9881C/GT911, ST7123 and ST7121 display revisions. The
speaker uses an ES8388 and the two microphones use an ES7210. The internal
antenna is selected at boot. Charging is enabled with Quick Charge disabled.

## Build and flash

Install and activate **ESP-IDF v6.0.1**, including its `esp32p4` tools, as in
the [SDK setup instructions](../README.md#1-install-esp-idf-v601).
From `esp32/`:

```sh
tools/muse/board.sh build tab5
```

Before pairing, enter your SDK token from **gadgets.muse.ai → Account → SDK
tokens** in **ESP32 Device SDK → Gadget SDK token**:

```sh
idf.py -B build-muse-m5stack-tab5 menuconfig
tools/muse/board.sh build tab5
```

The token stays in `build-muse-m5stack-tab5/sdkconfig`, which is ignored by
Git. Never add it to the board overlay or commit the generated configuration.

Connect the Tab5's USB-C port with a data cable. It enumerates as native
Espressif USB Serial/JTAG (`303a:1001`). Back up the original 16 MB flash
before the first installation:

```sh
python -m esptool --chip esp32p4 -p PORT read-flash 0 0x1000000 tab5-backup.bin
tools/muse/board.sh flash tab5 PORT
python tools/muse/monitor.py PORT 30
```

Replace `PORT` with the Tab5's serial port. Keep the backup outside Git. The
firmware targets P4 silicon older than v3, at 360 MHz, matching M5Stack's
factory configuration. Check `esptool chip-id` before using a board with a
different P4 revision. If flashing cannot enter download mode, follow
[M5Stack's Tab5 download-mode procedure](https://docs.m5stack.com/en/core/Tab5).

## C6 firmware

The C6 must run **ESP-Hosted 2.12.13 with Wi-Fi and Bluetooth enabled over
SDIO**, matching the P4 host dependency. The factory demo uses an older
ESP-Hosted release; do not assume its firmware supports Muse's BLE pairing.
Flashing the P4 does not update the C6.

Espressif publishes an
[`esp32c6-v2.12.13.bin` slave image](https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.13.bin)
and a
[`host_performs_slave_ota` example](https://github.com/espressif/esp-hosted-mcu/tree/v2.12.13/examples/host_performs_slave_ota).
An updater must use Tab5's SDIO wiring and enable its C6 power rail through
I/O expander `0x44`, pin 0, before starting ESP-Hosted. A generic P4 evaluation
board updater uses different pins. Do not install it unchanged on the Tab5.

| Signal | P4 GPIO |
|---|---|
| SDIO CLK | 12 |
| SDIO CMD | 13 |
| SDIO D0 / D1 / D2 / D3 | 11 / 10 / 9 / 8 |
| C6 CHIP_PU | 15; must be high after reset |

`CONFIG_ESP32P4_TAB5_C6_BOARD` selects this mapping in ESP-Hosted. Override
its reset default with `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`: the
2.x driver ends its reset pulse at the selected "active" level, and C6
CHIP_PU must finish high. Copying the factory's 1.x active-low setting
leaves the C6 in reset with this driver. Espressif describes the version
difference in [ESP-Hosted issue 244](https://github.com/espressif/esp-hosted-mcu/issues/244).
Muse's early board hook powers the C6 before Link requests its MAC address.
Transport DMA pools prefer PSRAM to avoid exhausting internal RAM before
the scheduler releases the startup stack memory.

## Controls and validation

Tap the screen to confirm pending pairing. Hold the on-screen microphone to
record a voice note, then release to send it. Use the touchscreen for settings
and tap it to wake the dimmed screen. RESET/BOOT is managed by the Tab5's
power controller; it is not wired as Muse's talk or factory-reset button.
The physical power button retains the board's hardware power controls.

The port builds with ESP-IDF v6.0.1. On an ST7121 Tab5, UART logs confirmed
boot, 32 MB PSRAM, panel detection, UI creation, 16 kHz stereo codec
initialization and BLE advertising. The C6 was upgraded from 1.4.1 to 2.12.13
over SDIO; its new version was verified after reboot.

Touch coordinates, app pairing, Wi-Fi connection/reconnect, microphone
channel order, actual recording/playback, charging and screen wake still
need hardware checks. Camera, SD card,
IMU, battery readings and software shutdown are not exposed by this port.
Images from Muse are unavailable on P4 because its ROM has no JPEG decoder.
The touchscreen and LVGL keep running while the backlight is off so a tap can
wake the display; battery standby consumption has not been characterized.
