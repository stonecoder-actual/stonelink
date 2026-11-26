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

Do not connect breadboard 5 V to the ESP32 5 V rail. Switch power off while wiring; verify polarity and voltage before connecting. Keep USB data wiring short. Firmware 0.5.0 defaults to manual external +5 V control; optional active-high GPIO21 control requires a suitable high-side switch. See POWER_TEST.md for the current manual test and DFR0457 wiring.

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

Camera-installed experiments are recorded in TEST_RESULTS.md. Camera-sourced VBUS, external supply backfeed paths and exclusive filesystem ownership remain unresolved. Firmware read-only access does not make concurrent camera writes consistent. Manual external +5 V cycling has restored access in reported tests; automatic high-side switching still needs physical validation.

## Onboard destination microSD (firmware 0.2.0)

User photos identify module marking ESP32-S3 N16R8, PCB V1695 / 231016, dual USB-C, CH343 UART and onboard microSD socket. Manufacturer branding is not established by photos. Board layout matches Freenove-style S3 WROOM CAM; initial SDMMC 1-bit mapping CMD38 / CLK39 / D0=40 is based on manufacturer reference https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html. Verify by successful card mount on this board; do not call this physically traced wiring.

Destination pictured: Gigastone 64 GB microSDXC. This firmware needs FAT16/FAT32; exFAT unsupported. No automatic formatting. Power ESP32 off before inserting/removing the card. The onboard slot supplies its own 3.3 V; do not wire the separate full-size MH-SD breakout to it. GPIO19/20 remain reserved for USB, GPIO38/39/40 for onboard SD; no camera sensor is initialized.

The source is the CL-CBL over Mini-USB. Destination is a separate card in the ESP32 socket. All archive output goes under /sd/stonelink, source paths remain /usb. Initial test mounts only; writes begin on explicit COPY/COPYALL commands. No automatic source deletion or VBUS control.

Firmware 0.3.0 intentionally skips onboard SD mounting at startup while replacement-card testing is deferred. No hardware wiring changes for PLAN/SCHEDULE. Local SD support remains available only via explicit SDMOUNT. PLAN ends with the source filesystem/device released but does not remove USB 5 V; that still requires the manual breakout supply switch in the current prototype.

Firmware 0.3.1 supersedes the earlier FAT32-only restriction: destination cards may use FAT32 or exFAT. Source may remain FAT32. Project-local FatFs override is used; no extra slot wires or automatic formatting. Manual SDMOUNT/SDSTATUS remains the first physical-card check. Aggregate capacities over 32 GB are supported in filesystem tests; physical capacity/card health must still be verified on device.

Firmware 0.5.0 reserves GPIO21 for an active-high high-side external supply switch; software defaults to MANUAL and does not drive it until explicitly selected. The original low-side module tested with an LED is unsuitable for switching USB supply in the shared-ground wiring. Follow POWER_TEST.md, including logic power/ground and a 10 kΩ signal pulldown for the future DFR0457. Removing external +5 V does not prove camera-supplied VBUS disappears. Keep UART connected to an awake laptop for complete logging. Insert/remove destination card only while ESP32 is off. No LoRa module is needed.
