# Mazda 2 Smart Gauge for ESP32 CYD (320x240)

Blue-black OBD-II gauge: Dash (6 tap-to-change values + CRP voltage), Data (12 values + all supported PIDs),
Graph, Trip, Fault codes (read/clear), Setup (Thai/English, demo/OBD, shift light, warnings, brightness).

## Get the .bin (no software to install)
1. Create a GitHub repository and upload these files at the top level: main.cpp, ui_strings.h, platformio.ini, README.md, plus the workflow in .github/workflows/.
2. Open the **Actions** tab, wait for "Build firmware" to finish (about 5 minutes).
3. Download the artifact: `mazda2-gauge-cyd` (one USB port, ILI9341) or `mazda2-gauge-cyd_st7789` (two USB ports).
4. Flash at address **0x0**: open https://espressif.github.io/esptool-js/ in Chrome, connect, add the file at 0x0, Program.
   (or: `python -m esptool --chip esp32 write_flash 0x0 mazda2-gauge-cyd.bin`)

## Build yourself
`pio run -e cyd -t upload` (PlatformIO).

## Using it
* Starts in **Demo** mode. Setup -> Data source -> OBD BT to connect to an ELM327 **Bluetooth Classic** adapter
  (names tried: OBDII, V-LINK, OBD2, Vgate, VEEPEAK, OBDLink, ELM327, iCar; PIN 1234). BLE-only adapters are not supported.
* Tap any of the 6 dash tiles to cycle through 16 values. Choices are remembered.
* Car name comes from the VIN (Mode 09). Mazda 2 is recognised from VIN chars 4-5 (DJ/DE/DW) - best effort.
* ATF temperature and oil temperature are not standard on most cars; they show "--" unless the car answers those PIDs.
  ATF needs a Mazda-specific Mode 22 PID that is not included.

## CRP voltage (crankshaft position signal)
OBD-II cannot give this. It is measured on GPIO35 (CYD P3 connector). The ESP32 ADC accepts 0-3.3 V only:
use a resistor divider (set `CRP_RATIO` in platformio.ini = (Rtop+Rbottom)/Rbottom). A variable-reluctance (AC) sensor
needs a conditioning circuit first. The value is an averaged level, not the waveform. Unconnected pin = noise.

## Troubleshooting
* Red/blue swapped or inverted colours: add `-DCYD_INVERT=1` or `-DTFT_RGB_ORDER=TFT_BGR` to build_flags.
* Touch offset: adjust the `map(p.x, 200, 3700 ...)` calibration in `loop()`.
* Thai text is pre-rendered bitmaps (ui_strings.h). To change wording edit gen_strings.py (in the full zip) and run it
  (needs `pip install pillow` with libraqm and a Thai font). PID names and fault-code descriptions stay English.
