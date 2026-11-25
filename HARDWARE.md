# Hardware

## Initial bench setup

Keep the adapter outside the camera. Leave the ESP32 native USB-C disconnected. Power/program the ESP32 through its USB-to-UART connector.

| Mini-USB breakout | Connection | Color |
| --- | --- | --- |
| GND | ESP32 GND and breadboard GND | White |
| D- | GPIO19 | Orange |
| D+ | GPIO20 | Blue |
| VBUS | Regulated breadboard 5 V | Red |
| ID | No external connection | None |

Do not connect breadboard 5 V to the ESP32 5 V rail. Switch power off while wiring; verify polarity and voltage before connecting. Keep USB data wiring short. No GPIO controls VBUS.

## Reported observations (prior experiments, not reproduced here)

- Adapter is FAT storage, 504,102,912 bytes, with DCIM, cell_hd, cell_std.
- GardePro X50S writes photographs and videos successfully.
- Mini-USB appears as Windows storage outside camera, disappears when inserted into camera.
- Laptop SD-reader access remains possible with original receiver attached.
- Receiver alone yielded approximately 0 V at exposed SD contacts; receiver is unreliable and not a project dependency.
- Camera-powered Mini-USB readings: VBUS 2.980 V; D- 0.917 V; D+ 2.905 V; ID/GND 0 V.
- Unpowered continuity reportedly joins ID/GND to identified SD grounds. SD pad numbering remains unverified.
- Prior analyzer captures show nearly simultaneous data-line transitions at startup; no significant capture-time activity.

## Unproven hypotheses

Shared internal storage, SD priority, sleep-time release, reset/power-cycle recovery, and proprietary switching remain hypotheses. Successful mounting does not establish safe concurrent camera access.

Before camera-installed testing, evaluate the measured camera-sourced VBUS and external supply backfeed path, and establish exclusive filesystem ownership. Firmware read-only access does not make concurrent camera writes consistent. No installed-camera or power-switch experiment is implemented yet.
