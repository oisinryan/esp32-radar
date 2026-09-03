# esp32-radar

A Wi-Fi and BLE radar scope for the **ideaspark ESP32 development board with the
integrated 1.9" ST7789 TFT (170×320)**.

Every access point and BLE advertiser in range becomes a contact on a sweeping
radar display. Nothing is transmitted — it only listens.

![mode](https://img.shields.io/badge/scan-Wi--Fi%20%2B%20BLE-brightgreen)
![board](https://img.shields.io/badge/board-ESP32--D0WD--V3-blue)
![display](https://img.shields.io/badge/display-ST7789%20170x320-orange)

---

## What it shows

| Element | Meaning |
|---|---|
| **Bearing** | Hashed from the BSSID / BLE address, so a contact holds the same angle between sweeps instead of jumping about |
| **Radius** | Signal strength. Strong sits near the centre, −95 dBm at the rim |
| **Green blip** | Wi-Fi access point, encrypted |
| **Red blip** | Wi-Fi access point, **open / unencrypted** |
| **Amber blip** | BLE advertiser |
| Dim blip | Contact resting between sweep passes |
| Very dim | Not seen for 20 s — going stale |

Four labelled range rings, a trailing sweep, and a strongest-first contact list
along the bottom of the panel.

**BOOT button** cycles `BOTH → WI-FI → BLE`.

## Serial commands

Connect at **115200**:

```bash
arduino-cli monitor -p /dev/cu.usbserial-210 -c baudrate=115200
```

| Key | Action |
|---|---|
| `d` | Dump the full contact register once |
| `c` | Toggle dumping after every sweep |
| `h` | Help |

The dump lists type, bearing, RSSI, age, name and address, sorted strongest
first. `BRG` matches the on-screen bearing (0° = right, clockwise), so any blip
can be traced back to a device.

The dump is on demand rather than continuous on purpose: a full listing runs to
~100 lines and `Serial.printf` blocks, which at 115200 stalls the sweep for
roughly half a second.

## Hardware

Board: **ESP32-D0WD-V3** (WROOM class, dual core, **no PSRAM**), 4 MB flash,
CH340 USB-C bridge, with the 1.9" ST7789 panel wired on the PCB.

Panel pins are fixed by the board — they are not a choice:

| Signal | GPIO |
|---|---|
| SCL / SCK | 18 |
| SDA / MOSI | 23 |
| CS | 15 |
| DC / RS | 2 |
| RST | 4 |
| Backlight | 32 |
| BOOT button | 0 |

Verified against both the vendor manual and the LCD FPC silkscreen
(`RST D4 · SCL D18 · CS D15 · RS D2 · SDA D23 · LEDA D32`).

## Build

Requires the `esp32` Arduino core, plus **LovyanGFX** and **NimBLE-Arduino**.

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashFreq=80,CPUFreq=240,DebugLevel=none" .
arduino-cli upload -p /dev/cu.usbserial-210 --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashFreq=80,CPUFreq=240,DebugLevel=none,UploadSpeed=115200" .
```

Notes on those flags:

- **`min_spiffs`** — the sketch is ~1.2 MB and does not fit the default 1.2 MB
  app slot comfortably. No filesystem or OTA is needed here.
- **`UploadSpeed=115200`** — this CH340 is unreliable at 460800 and above.
- The device node alternates between `/dev/cu.usbserial-10` and
  `-210` between plug-ins. Use whichever is present.

## Things that cost real time

Recorded because none of them are obvious, and each presented as something else.

### `setColorDepth(8)` is not an 8-bit palette

In LovyanGFX, `rgb332_1Byte = 8` while `palette_8bit = 8 | has_palette`, and
`createSprite()` only builds a palette when that bit is set. With plain `8` the
sprite is RGB332, `setPaletteColor()` returns silently, and drawing with palette
indices 0–47 renders them as RGB332 values that are all but black.

**The result is a completely blank panel while the firmware runs perfectly** —
frame rate, heap and scan counts all healthy on serial. Use
`fb.setColorDepth(lgfx::palette_8bit)`.

### `setPaletteColor(index, color)` reads integers as RGB888

The templated overload runs `convert_to_rgb888()` on its argument, so handing it
an RGB565 value blackens the palette. Use the explicit
`setPaletteColor(index, r, g, b)` form.

### A 16-bit full-screen buffer does not fit beside Wi-Fi and NimBLE

170×320 at 16 bpp is 108,800 bytes and leaves roughly 13 KB of heap. NimBLE
calls `abort()` the instant a scan starts, producing a crash loop. The 8-bit
palette buffer is 54,400 bytes and leaves ~65 KB, which is stable.

### BLE addresses rotate, so a shared contact table starves Wi-Fi

Privacy addresses mean a busy room delivers dozens of brand-new keys every
sweep. With one shared table and least-recently-seen eviction, every Wi-Fi AP is
evicted within a single pass and the scope shows nothing but ephemeral BLE
contacts. Hence the per-type quotas (`CAP_WIFI` / `CAP_BLE`).

### Watch for stray processes on the serial port

A serial monitor or any script holding `/dev/cu.usbserial*` makes esptool fail
with *"device reports readiness to read but returned no data (multiple access on
port?)"*, which reads like a bad cable. Check `lsof /dev/cu.usbserial*` first.

## tools/panel-bisect

A standalone sketch that cycles eight LovyanGFX panel configurations, four
seconds each, drawing a large numbered pattern with corner markers. When a panel
appears dead, flash this and note which variant looks right — far quicker than
guessing at config.

The confirmed-good configuration for this board:

```
VSPI_HOST, spi_mode 0, 40 MHz, spi_3wire true,
panel_width 170, panel_height 320,
memory_width 240, memory_height 320,
offset_x 35, offset_y 0,
invert true, rgb_order false
```

`offset_x = 35` is the value that actually matters — 170 centred on the
ST7789's 240-wide controller RAM. Variants with mode 3, 3-wire off or 20 MHz all
displayed correctly; only `offset_x 0` was visibly wrong.

## Tuning

| Constant | Default | Notes |
|---|---|---|
| `MAXB` | 128 | Contact table size, ~52 B each |
| `CAP_WIFI` / `CAP_BLE` | 56 / 72 | Per-type quotas, must sum to ≤ `MAXB` |
| `setMaxResults` | 100 | BLE results per scan, roughly 250 B each — the main heap cost |
| `BLE_MS` | 3000 | BLE scan window |

Keep an eye on the `heap` figure in the serial status line. It sits around
60–65 KB in normal use; if it falls toward 15 KB, NimBLE will abort.

## Credits and third-party licences

The firmware in this repository is original work, but it does nothing without
the following, and all credit for the heavy lifting belongs to their authors:

| Project | Author | Licence |
|---|---|---|
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | lovyan03 | FreeBSD (2-clause BSD) |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | h2zero | Apache-2.0 |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | Espressif Systems | LGPL-2.1 |

LovyanGFX's licence file additionally carries the notices of the work it
descends from — Adafruit Industries (Adafruit_ILI9341, BSD/MIT) and Bodmer
(TFT_eSPI, FreeBSD) — and those notices travel with the library.

The ST7789 panel configuration in this repository was derived by testing on
hardware, but the vendor pin definitions were published by **ideaspark** in
their board documentation.

None of the above authors are affiliated with this project or endorse it.

## Licence

MIT — see [LICENSE](LICENSE). This covers the original code here only; the
dependencies above remain under their own licences.
