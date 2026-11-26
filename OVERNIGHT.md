# Overnight soak test — 0.4.0 procedure, updated for 0.5.0 below

**Firmware 0.5.0 changes power handling:** follow [POWER_TEST.md](POWER_TEST.md)
first. SOAK and SCHEDULE now run the external-power workflow. Default MANUAL
mode pauses for POWER OFF/ON confirmations, including a final OFF acknowledgment;
it cannot collect unattended. Select GPIO mode only after installing/testing the
correct high-side switch. SOAK settings still restore after reboot, but power
mode always resets to MANUAL. Stop restored SOAK before the initial bench test.
The historical acquisition-only and unchanged-VBUS descriptions below apply to
0.4.0, not 0.5.0. Logger setup, destination checks and integrity limits still apply.

Purpose: log every UART line on the laptop, track resets/USB/media failures and heap/stack health, and optionally copy media to local FAT32/exFAT SD with checksum verification. No Wi-Fi or LoRa. Source files are never deleted or formatted.

## What this hardware can and cannot test

Demonstrated: camera armed idle plus external USB 5 V can allow reads; camera activity can stall USB; manual USB 5 V cycling has restored access. Current hardware has no GPIO-controlled power switch. The firmware does NOT pretend to power-cycle USB. Overnight failures may therefore persist until morning. This is useful failure/soak evidence, not proof of autonomous collection. Never connect external breakout 5 V to ESP32's own 5 V rail; retain the tested UART/data/GND wiring. Keep the setup dry and on a stable surface. Use a charged camera battery; do not change its power arrangement as another unrecorded variable tonight.

## 1. Upload and preflight while present

1. Close every Serial Monitor/logger using COM6. Power camera/breakout off. Insert destination card only with ESP32 power off; then reconnect UART power.
2. In project PowerShell:

```powershell
$pio = 'C:\Users\Sam\.platformio\penv\Scripts\platformio.exe'
& $pio run -t upload --upload-port COM6
```

3. Start the logger (not PlatformIO Serial Monitor at the same time):

```powershell
& 'C:\Users\Sam\.platformio\penv\Scripts\python.exe' -u tools/overnight_log.py --port COM6
```

Type firmware commands into this terminal and press Enter. Every command sent and every incoming line receives a host UTC timestamp and is recorded. This logger opens with DTR/RTS deasserted and automatically reconnects without deliberately resetting the ESP32. Some reader/drivers may still behave differently; resets are logged.

4. With breakout 5 V off, arm camera; wait until ready for motion detection and screen off. Remain outside the detection area and enable breakout regulated 5 V. Keep camera/card/data wiring unchanged.
5. For local-copy mode, run SDMOUNT then SDSTATUS. Require a successful mount and sensible free space. Firmware never formats. Run one COPY using a small real path from SCAN, e.g. COPY /usb/cell_std/100MEDIA/DSCF0001.JPG. Require COPY VERIFIED, then repeat to require COPY SKIP verified. If no real local mount/copy passes, use monitor-only mode tonight.

## 2. Choose one overnight mode

Monitor only (no local card needed):

```text
SOAK ON
```

First media scan begins in 30 seconds, subsequent cycles five minutes after prior completion. Each cycle mounts source fresh, runs PLAN media diff, unmounts/releases source. Heartbeat every minute. No file data copies.

Verified copy mode (local card required):

```text
SOAK COPY
SOAK EVERY 300
```

SOAK COPY defaults to a six-hour interval for the intended four-window daily cadence. EVERY 300 deliberately changes this to five-minute intervals for tonight's stress test; first cycle is then five minutes away. SOAK NOW runs one immediately for preflight. Use SOAK STATUS to inspect the next uptime deadline. Wait for an end=... result=SUCCESS and health line before leaving the test. Mode 2 success means the current COPYALL traversal finished with new files verified/existing files reverified; it is not a claim a new camera photo exists each cycle.

SOAK COPY writes only /sd/stonelink. Archive names contain source path, byte size and CRC32; reused names with different content are retained separately. Every new copy is flushed, reopened and checked. Interrupted copies keep a reserved partial staging file for the next attempt, with source untouched. Long copy/hash operations delay console/heartbeats until they finish; USB library/MSC tasks keep running. Motion can interrupt a batch, which records FAILED. Source capacity is approximately 504 MB, so full batches can take many minutes. No automatic source deletion or continuous filesystem-sharing guarantee.

If local SD is mounted, /sd/stonelink/soak.csv also appends structured cycle/health records, fsynced per record. CSV columns: boot uptime milliseconds,event,cycle,result,detail (semicolon-separated key/value fields). The laptop UART log is the only complete raw log, including driver errors/descriptors. Local journal does not capture every UART line and is not a replacement for laptop capture.

## 3. Leave logging running

- Keep laptop plugged in and prevent system sleep/hibernation while plugged in; screen-off is fine. Logger cannot capture while Windows sleeps. Do not close the terminal or unplug UART.
- Keep the camera armed. Note approximate clock times of deliberate motion events, preferably let normal activity occur.
- Do not enable WATCH or SCHEDULE alongside SOAK. SOAK disables them when armed and suppresses unrelated diagnostic acquisitions; each soak cycle tries acquisition once then waits its interval. Do not run RESET loops or automatically reboot on failures.
- SOAK mode/interval are saved to NVS and resume after ESP32 reset (30-second startup delay), provided NVS initialization/commit succeeded. Counters and PLAN baseline reset per boot; host logger preserves session totals. No NVS repair erase is performed. Verify persistent=1 and no NVS save error. Settings are not a real-time clock schedule.

Logs are created under ignored logs/ with a unique timestamp prefix:

- .log — all serial lines and logger events with UTC timestamps.
- .jsonl — structured equivalent, including errors, commands and reconnects.
- .summary.json — refreshed every 30 seconds and at clean exit, event counts and latest health/cycle/boot records.

Summary firmware_error is a count of error-level lines, not independent hardware failures. cycle_failed is a count of failed windows, not lost/corrupted files. Boot count includes initial observed boot. A skipped already verified archive file is not a new transfer.

## 4. Morning shutdown and review

Enter SOAK STATUS, then SOAK OFF. Wait for mode=0 and no persistence error. OFF is saved across resets. Ctrl+C exits the laptop logger but DOES NOT turn SOAK off. Do not remove destination SD until camera/source/breakout are off and ESP32 powered off.

Send .summary.json plus representative first failure and recovery lines (or provide the complete .log file). Retain local CSV and archive files. Check camera playback/new-file counts and independently open a copied photo/video on PC. Compare byte counts/checksums before calling integrity verified. No absence-of-errors claim is justified across missing log intervals or laptop sleep.

## Validation performed before delivery

Firmware compiles; host logger unit/integration tests cover event classification, UTC, partial-line handling, reconnect, one-time commands and final summary. Production collection policy and FAT32/64 GiB exFAT simulated-disk tests pass. Real microSD mount/copy, NVS reboot restoration, overnight stability and powered hardware recovery still require this physical test. No overnight run or upload has been performed by the agent.
