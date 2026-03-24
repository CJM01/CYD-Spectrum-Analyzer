# CYD Spectrum Analyzer

- Made with Claude AI
- A real-time audio spectrum analyzer optimized for **guitar frequencies (80 Hz – 5 kHz)**, running on the **Hosyond 4" ESP32-3248S040 CYD** with an **INMP441 I2S microphone**. Displays a live bar graph with a dark purple → yellow gradient on the built-in ST7796 TFT display.

![Spectrum Analyzer]([docs/screenshot_placeholder.png](https://github.com/CJM01/CYD-Spectrum-Analyzer/blob/9b25c8a361d4771f4de1942cfbbaf8206d95302d/docs/Screenshot.jpg?raw=true))

---

## Features

- 48 log-spaced frequency bars from 80 Hz to 5 kHz
- Dark purple (quiet) → yellow (loud) colour gradient
- Peak-hold dots with gravity fall-off
- Per-frame DC offset removal via high-pass IIR filter (fixes INMP441 bias issue)
- Temporal bar smoothing for a fluid, non-spiky display
- Serial debug output showing dominant frequency and dB once per second
- Two diagnostic tools included for mic troubleshooting
- Originally made to monitor guitar sounds

---

## Hardware

| Component | Part |
|---|---|
| Microcontroller | [Hosyond 4" ESP32-3248S040 CYD](https://www.lcdwiki.com/4.0inch_ESP32-32E_Display) |
| Display | ST7796 480×320 TFT (built-in) |
| Microphone | INMP441 I2S MEMS microphone |

### Wiring — INMP441 to ESP32

| INMP441 Pin | ESP32 Pin | Notes |
|---|---|---|
| VDD | 3.3V | Power |
| GND | GND | Ground |
| **L/R** | **GND** *(on the mic itself)* | Selects Left channel — must be tied to GND |
| SCK | IO32 | I2S Bit Clock (output) |
| WS | IO25 | I2S Word Select (output) |
| SD | IO35 | I2S Serial Data (input-only pin) |

> **Why IO35 for SD?** IO35 is input-only on the ESP32, making it perfect for mic data. It cannot accidentally drive the line, which protects the mic.
>
> **Why avoid the SPI header (IO23/IO19/IO18)?** Those pins are shared with the SD card slot on this board. Using them for the mic would conflict with SD card access.

---

## Software Requirements

### Arduino IDE Libraries
Install these via **Sketch → Include Library → Manage Libraries**:

| Library | Author | Notes |
|---|---|---|
| **TFT_eSPI** | Bodmer | Display driver |
| **arduinoFFT** | Enrique Condes | **Must be v2.x** — API changed from v1 |

### Board Package
- **esp32** by Espressif Systems (via Boards Manager)
- Board: `ESP32 Dev Module`
- Partition Scheme: `Default 4MB (no OTA)`

### TFT_eSPI User_Setup.h
You **must** configure TFT_eSPI for this board before compiling. Replace the contents of `User_Setup.h` in your TFT_eSPI library folder with:

```cpp
#define USER_SETUP_INFO "User_Setup"
#define ST7796_DRIVER
#define TOUCH_CS 33
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
```

---

## Project Structure

```
CYD-Spectrum_Analyzer/
│
├── AudioSpectrumAnalyzer/
│   └── AudioSpectrumAnalyzer.ino   ← Main sketch
│
├── tools/
│   └── INMP441_MicTest.ino     ← Mic health diagnostic
│   └── INMP441_ShiftCal.ino    ← Bit-shift calibration tool
│
└── docs/
    └── TROUBLESHOOTING.md
```

---

## Quick Start

1. Wire the INMP441 as shown above
2. Configure `User_Setup.h` in TFT_eSPI
3. Install both libraries
4. Open `AudioSpectrumAnalyzer/AudioSpectrumAnalyzer.ino` in Arduino IDE
5. Select your board and port, then upload
6. Open Serial Monitor at **115200 baud** to see per-second frequency debug output

---

## Tuning Parameters

All tuning is done via `#define` constants near the top of the sketch:

| Define | Default | Effect |
|---|---|---|
| `SAMPLE_RATE` | `10000` | Raise to `20000` for 10 kHz range |
| `DB_FLOOR` | `50.0` | Noise floor — raise if bars dance in silence |
| `DB_SCALE` | `80.0` | Dynamic range — lower if bars never reach the top |
| `FALL_SPEED` | `4` | Pixels dropped per frame; higher = snappier |
| `PEAK_HOLD_FRAMES` | `25` | How long peak dots linger |
| `DC_ALPHA` | `0.98` | DC filter speed (0.95–0.99); lower = faster tracking |
| `SMOOTH_UP` | `0.3` | Bar rise smoothing (0=instant, 1=frozen) |
| `SMOOTH_DOWN` | `0.6` | Bar fall smoothing; raise for less spiky display |

---

## How It Works

### I2S Audio Capture
The INMP441 is a digital MEMS microphone that outputs 24-bit audio over I2S. The ESP32's I2S peripheral clocks the mic at 10 kHz sample rate. Samples arrive as 32-bit left-justified words; we right-shift by 8 bits to get the 24-bit value.

### DC Offset Problem (and Fix)
The INMP441 has a natural DC bias — the raw signal doesn't sit at zero. Feeding biased audio into an FFT causes a massive spike at 0 Hz that "smears" (spectral leakage) across every frequency bin, drowning out the actual guitar signal. 

The fix is a **first-order high-pass IIR filter** applied to every sample before the FFT:
```
dcEstimate += DC_ALPHA * (sample - dcEstimate)
filtered    = sample - dcEstimate
```
With `DC_ALPHA = 0.98` this tracks and removes DC within ~5ms, while leaving 80 Hz and above completely untouched.

### FFT and Log-Spaced Bars
1024 samples are windowed with a **Hann window** (reduces spectral leakage from the window edges) and run through an FFT
The 512 output bins are mapped to 48 display bars using **logarithmic spacing** — so the bar width in Hz grows with frequency, matching how humans perceive pitch. Without log spacing, all guitar fundamentals would be crammed into the left 10% of the screen.

### Temporal Smoothing
Raw FFT frames are noisy frame-to-frame. Each bar's magnitude is blended with the previous frame:
- **Rising:** `smoothed = smoothed × 0.3 + new × 0.7` (fast attack)  
- **Falling:** `smoothed = smoothed × 0.6 + new × 0.4` (slow decay)

This is the same technique used in commercial spectrum analyzers.

---

## Diagnostic Tools

If the analyzer isn't responding or looks wrong, use the tools in `/tools` first.

### INMP441_MicTest
A health check that runs every 500ms and shows on both Serial and TFT:
- Raw min/max sample values
- RMS amplitude and peak-to-peak swing  
- dBFS level with a visual bar
- Automatic verdict: DEAD / STUCK / DC OFFSET / WEAK / OK

**Verdicts and causes:**

| Verdict | Likely Cause |
|---|---|
| `DEAD: all zeros` | VDD or GND disconnected, or SD pin not connected |
| `STUCK: tiny swing` | SCK or WS not reaching mic — clock not running |
| `DC OFFSET only` | L/R pin floating (must be tied to GND on the mic) |
| `WEAK: very low signal` | Wiring OK but signal very quiet |
| `OK - mic responding` | All good |

### INMP441_ShiftCal
Tests bit-shifts 6–14 on live mic data and scores each one by DC/RMS ratio and dBFS. Useful if you suspect the I2S data alignment is wrong. Results print to Serial and the winner is highlighted in green on TFT.

> **Note:** On this board with this driver configuration, the correct shift is `>> 8`. The DC/RMS ratio will be identical across all shifts (they're just scaled versions of each other) — what matters is that the DC mean is small relative to RMS, which the IIR filter in the main sketch handles automatically.

---

## Serial Output

With Serial Monitor open at 115200 baud, the main sketch prints once per second:
```
Peak bar=22 (~532 Hz)  mag=8082.2  db=78.2  DC=69738.0
```
- **Peak bar** — which of the 48 bars is currently loudest
- **~Hz** — approximate centre frequency of that bar
- **mag** — smoothed FFT magnitude
- **db** — magnitude in dB (log scale)
- **DC** — current DC estimate (should be large but stable; the filter removes it)

---

## License

MIT — free to use, modify, and share.
