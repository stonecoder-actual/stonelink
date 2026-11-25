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

## Onboard destination microSD (firmware 0.2.0)

User photos identify module marking ESP32-S3 N16R8, PCB V1695 / 231016, dual USB-C, CH343 UART and onboard microSD socket. Manufacturer branding is not established by photos. Board layout matches Freenove-style S3 WROOM CAM; initial SDMMC 1-bit mapping CMD38 / CLK39 / D0=40 is based on manufacturer reference https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html. Verify by successful card mount on this board; do not call this physically traced wiring.

Destination pictured: Gigastone 64 GB microSDXC. This firmware needs FAT16/FAT32; exFAT unsupported. No automatic formatting. Power ESP32 off before inserting/removing the card. The onboard slot supplies its own 3.3 V; do not wire the separate full-size MH-SD breakout to it. GPIO19/20 remain reserved for USB, GPIO38/39/40 for onboard SD; no camera sensor is initialized.

The source is the CL-CBL over Mini-USB. Destination is a separate card in the ESP32 socket. All archive output goes under /sd/stonelink, source paths remain /usb. Initial test mounts only; writes begin on explicit COPY/COPYALL commands. No automatic source deletion or VBUS control.
