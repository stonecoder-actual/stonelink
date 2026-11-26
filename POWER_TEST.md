# External USB power workflow — firmware 0.5.0

0.7.0 update supersedes the naming notes below: archive filenames now carry
PHOTO/VIDEO and size-based SD/HD flags, grouped by ESP and exact camera time.
They can be reclassified as matching files arrive. HD requests use the archived
filename and persist locally. See MEDIA_ARCHIVE.md; no radio sender is attached.

0.6.0 update: power procedures are unchanged. New copies use factory-MAC ESP
prefixes, embedded camera dates, SHA-256 and JSON records; see MEDIA_ARCHIVE.md.
Old CRC-layout copies remain and are not automatically migrated. First copy
under the new layout may copy again before future repeats can verify/skip it.

0.5.1 update: discovery and copying accept only .mp4, .jpg and .jpeg, ignoring
case. Non-media files such as sync0.dat are ignored before checksum reads and
reported as ignored=N; skipped=N still means verified existing media. Existing
archived non-media files are retained. Power/manual procedures below are unchanged.

COLLECT, SCHEDULE and SOAK now share an external-supply cycle. This supersedes
the 0.4.0 soak's acquisition-only behavior. The ESP has no camera recording or
standby sensor: start bench cycles after recording finishes. Motion during
access can still interrupt a cycle; exclusive SD ownership is not established.
No source deletion, formatting, Wi-Fi or LoRa is added.

## Manual bench test

Keep the existing working data wiring (GPIO19 D-, GPIO20 D+) and common grounds.
Remove the LED-test MOSFET module from this circuit. Remove/reconnect only the
regulated external +5 V wire feeding breakout VBUS. Do not connect this supply
to ESP32's 5 V rail. ESP32 stays powered through USB-to-UART; native USB-C stays
disconnected. Camera/card remain connected during the test.

1. Close COM6 monitors/loggers, upload 0.5.0, then open UART logger or monitor.
2. Send SOAK OFF immediately to stop any restored old soak configuration. Send
   SCHEDULE OFF, then POWER MODE MANUAL. MANUAL is also the default every boot;
   GPIO21 is untouched by this controller until GPIO mode is explicitly enabled.
3. Let the camera reach standby and finish recording. Run COLLECT PLAN.
4. Wait for POWER ACTION asking to disconnect +5 V. Pull that positive wire,
   then send POWER OFF. This confirms your action, not a hardware command in
   manual mode. The ten-second off hold starts at your acknowledgment.
5. Wait for POWER ACTION asking to reconnect +5 V. Reconnect it, then send
   POWER ON. Early ON confirmations are rejected.
6. After five seconds settling, the controller waits up to 30 seconds for USB
   presence, then tries acquisition at most three times, five seconds apart.
7. Expect PLAN BASELINE entries and PLAN complete on the first successful scan.
   After source release, a final POWER ACTION asks for OFF: pull +5 V again and
   send POWER OFF. Expect COLLECT end=... result=SUCCESS. Mounted=0 is intentional.
8. Capture one new photo/video, wait for recording to finish, repeat steps 3–7.
   Look for PLAN NEW or CHANGED. Metadata discovery does not verify file bytes.

```powershell
& 'C:\Users\Sam\.platformio\penv\Scripts\platformio.exe' run -t upload --upload-port COM6
& 'C:\Users\Sam\.platformio\penv\Scripts\python.exe' -u tools/overnight_log.py --port COM6
```

Do not paste POWER OFF/ON in advance. Acknowledgments do not measure voltage or
detect your action. Camera-sourced VBUS can remain when the external wire is
removed. This is an external-supply cycle, not proof of full adapter power loss.
Fresh disconnect/enumeration is not mandatory if camera power keeps USB alive.
Automatic diagnostic mounting is suppressed during and after managed workflows.

## Commands and failures

| Command | Behavior |
| --- | --- |
| COLLECT or COLLECT PLAN | One power-cycle/metadata-plan/release cycle |
| COLLECT COPY | One power-cycle/verified COPYALL/release; local mount must pass first |
| POWER STATUS | Mode, phase, acquisition attempts, uptime deadline; no voltage sensing |
| POWER OFF / ON | Acknowledge the corresponding manual prompt only |
| POWER CANCEL | Abort cycle; GPIO commands OFF; manual users must remove external +5 V themselves |
| POWER MODE MANUAL | Human confirmations; release GPIO21 if previously configured |
| POWER MODE GPIO | Explicitly enable active-high GPIO21 control, initially LOW |

Prompts expire after five minutes, with reminders every 30 seconds. Timeout or
cancel never counts as success, even if file work completed. COLLECT data result
reports file work separately; COLLECT end also requires final supply release.
Missing USB, exhausted acquisition attempts, scan/copy failure or GPIO errors
yield FAILED. No automatic reboot, BOT-reset loop or indefinite retry occurs.
Cleanup failure stops the workflow without changing supply. Inspect logs and
try REINIT; if release still fails, reset the ESP with its button.

STATUS, HELP, POWER commands and SOAK OFF remain available during timed waits.
Conflicting diagnostic/filesystem commands are rejected while a workflow runs.
Transfers/driver calls remain synchronous: console input/cancel waits for those
calls to return. USB workers continue separately. Explicit RETRY still permits
legacy direct SCAN/READ tests; PLAN alone remains an immediate scan without a
power cycle. Source files remain write-protected throughout.

## Copy and scheduling

Verify a known good destination with SDMOUNT/SDSTATUS before COLLECT COPY. It
archives supported MP4/JPEG files under /usb using the existing CRC/size verification,
staging, fsync, reopen and verified-skip policy. No camera files are deleted.
Local mount failure ends the cycle before any power request. Independently open
a copied image/video on a PC before relying on the archive.

SOAK ON = metadata, five-minute interval; SOAK COPY = verified archive, six-hour
interval. SOAK EVERY 300 selects a five-minute stress interval after choosing
ON/COPY. SOAK NOW starts immediately; SOAK OFF cancels its pending workflow and
stops future cycles. SCHEDULE remains metadata-only but now uses the same power
workflow, with its existing bounded three-window retry policy. SOAK owns timing
when enabled; stop it before SCHEDULE or one-shot COLLECT. Workflows do not overlap.

MANUAL scheduled/soak cycles require someone present. They are not autonomous
overnight collection. SOAK mode/interval still persist in NVS; power-control mode
does NOT persist and always returns to MANUAL after reset. The UART logger now
classifies power prompts/errors/timeouts and COLLECT results separately from
SOAK results. Optional local journal includes power transitions and health.

## Future DFRobot DFR0457 wiring

Only for the actual DFR0457 high-side module, not the low-side LED-test module.
Wire with supplies off:

| DFR0457 connection | Prototype connection |
| --- | --- |
| Power VIN | External regulated +5 V |
| Power GND | Supply ground, ESP ground and breakout ground |
| Power VOUT | Breakout VBUS/5 V; remove direct +5 V bypass wire |
| Gravity logic VCC (+) | ESP 3.3 V |
| Gravity logic GND (-) | ESP ground |
| Gravity signal (S) | ESP GPIO21 |

A 10 kΩ resistor from signal to logic GND defines OFF during reset/boot while
GPIO21 is an input. Follow markings, not cable colors. GPIO19/20 remain USB;
GPIO38/39/40 remain local SD. First verify the new switch with an isolated
LED/resistor load. Then stop SOAK, select POWER MODE GPIO, test COLLECT PLAN
while present. Same off/settle workflow, without human prompts; final release
commands GPIO21 LOW. Re-enable GPIO mode explicitly after each reset. Camera
power paths, backfeed and repeated capture/recovery still require measurements.

Reference: [DFRobot specifications, pinout and schematic](https://wiki.dfrobot.com/dfr0457/).

## Software tests

tests/power_cycle_test.c runs the production controller with simulated time,
USB presence and a recording/failing output callback. It covers no GPIO writes
in MANUAL, prompt order, timing boundaries, bounded retries, device/manual
timeouts, early/late confirmations, cancellation, output failures, final release
and no supply change on MSC cleanup fault. These tests do not validate real USB
electrical behavior or GPIO wiring.

```powershell
& '.\.pio\host-tools\tcc\tcc.exe' -Isrc -run tests/power_cycle_test.c
```
