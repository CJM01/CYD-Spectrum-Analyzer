# Troubleshooting Guide

## Display Issues

### Blank / white screen
- Verify `User_Setup.h` matches the configuration in the README exactly
- Confirm the board is set to **ESP32 Dev Module** in Arduino IDE
- Try reducing `SPI_FREQUENCY` to `20000000` in `User_Setup.h`

### Garbled display or wrong colours
- The ST7796 driver must be selected (`#define ST7796_DRIVER`), not ILI9341 or others
- Make sure `TFT_DC` is set to `2`, not another pin

---

## Microphone Issues

### All bars stuck at zero / no response to sound
Run `INMP441_MicTest` first. Look at the verdict:

**DEAD (all zeros)**  
- Check VDD is connected to 3.3V (not 5V — the INMP441 is a 3.3V device)
- Check GND is connected
- Check the SD pin is connected to IO35
- Try reseating all connections

**STUCK (tiny swing)**  
- SCK (IO32) or WS (IO25) is not reaching the mic
- Check those two connections; a missing clock means the mic never sends data

**DC OFFSET only**  
- The L/R pin on the mic itself is floating
- It must be tied directly to GND **on the mic breakout board**, not through a long wire

### Bars respond but are always maxed out
Lower `DB_FLOOR` — try raising it to `55.0` or `60.0`. The noise floor of your environment may be higher than the default.

### Bars barely move even with loud guitar
Lower `DB_FLOOR` to `25.0` and lower `DB_SCALE` to `50.0`. Check that the mic is within 30cm of the guitar body or amp.

### Display is spiky / single bars jumping high randomly
Raise `SMOOTH_DOWN` toward `0.75`. If DC is swinging wildly (visible in serial output), lower `DC_ALPHA` toward `0.95`.

---

## Compilation Issues

### `FFT.windowing()` errors / wrong argument types
You have arduinoFFT **v1.x** installed. The main sketch uses the **v2.x API**. Open Library Manager, find arduinoFFT, and update to the latest version.

### `i2s_config_t` or `i2s_pin_config_t` not found
Make sure you have the **esp32 board package by Espressif** installed, not a third-party package. The `driver/i2s.h` header is part of the Espressif IDF.

### TFT_eSPI compile errors about missing defines
The `User_Setup.h` file in the library folder needs to be updated — see README for the correct contents. The library ships with a generic setup that doesn't match this board.

---

## Understanding the Serial Output

```
Peak bar=22 (~532 Hz)  mag=8082.2  db=78.2  DC=69738.0
```

- **DC value is large** — This is normal. The INMP441 has a natural DC bias of tens of thousands of counts. The IIR filter removes it before the FFT. The DC value in serial output is the *raw* estimate before removal.
- **DC value changes sign rapidly** — The filter is chasing a very unstable signal. Try lowering `DC_ALPHA` to `0.95`.
- **db values always the same** — The signal may be clipping. Raise `DB_SCALE` or move the mic further from the source.
- **Peak bar jumps all over** — Normal during strumming. The peak tracks the loudest frequency each second, and guitar chords have energy across many harmonics.
