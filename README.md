# StoneLink media collector

Firmware 0.8.0 receives new media into a durable local inbox, releases USB and
finishes the external OFF workflow, then verifies and organizes it on the ESP SD.
Default FAST mode skips indexed files by source path, size and modification time;
it does not reread old media. New/changed content precedes old recovery/audit work.
See [MEDIA_ARCHIVE.md](MEDIA_ARCHIVE.md#receive-first-pipeline-080) for the bench test.

Media tracking remains directly in camera-independent filenames:
`<ESP-prefix>_<camera-date>_<camera-time>_PHOTO-or-VIDEO_SD-or-HD_<sha256>`.
Files group by ESP, exact camera clock and type. A lone file stays SD; with
multiple distinct versions, a uniquely larger file becomes HD and smaller files
remain SD. Later arrivals reconcile existing flags. HD requests can use an
archived filename and persist on local SD; no transport is attached yet.
See [MEDIA_ARCHIVE.md](MEDIA_ARCHIVE.md) for rules, edge cases and bench commands.

ESP32-S3 USB host experiment for the SPYPOINT CL-CBL adapter, with verified local FAT32/exFAT archiving. Media collection accepts only MP4 and JPEG files (.mp4/.jpg/.jpeg, case-insensitive), using the shared external USB power workflow for one-shot collection, scheduling and soak tests. Default MANUAL mode pauses for human power confirmations; opt-in GPIO mode controls a suitable active-high switch on GPIO21. Source deletion and radio transmission remain unimplemented. Automatic-switch validation remains bench work.

The media allowlist applies to COPY, COPYALL, COLLECT COPY, SOAK COPY and PLAN/SCHEDULE inventories. Other regular files are ignored before hashing or copying and counted as ignored=N, separately from skipped=N (already verified media). Existing archived non-media files are retained; the filter never deletes them. Firmware-generated local logs and staging files are unaffected.

Start with [POWER_TEST.md](POWER_TEST.md) for the current manual-power test, commands and future switch wiring. Versioned sections below record earlier behavior; 0.5.0 power/scheduler behavior supersedes their no-power-control descriptions.

## Build and run

Power/program through USB-to-UART; leave native USB-C disconnected. Insert/remove the local microSD only with ESP32 power off. Then use PowerShell:

```powershell
$pio = 'C:\Users\Sam\.platformio\penv\Scripts\platformio.exe'
& $pio run
& $pio run -t upload --upload-port COM6
& $pio device monitor --port COM6 --baud 115200
```

Close Serial Monitor before upload. COM6 was identified as CH343 VID:PID 1A86:55D3, serial 5CCC047926; confirm if Windows changes the port. Upload is a separate user action.

Pinned dependencies: Espressif32 6.10.0, ESP-IDF 5.4.0 (3.50400.0), usb_host_msc 1.1.4 and dependencies.lock. Physical flash configured for 16 MB; default application partition is 1 MiB. PSRAM remains unused.

## Local microSD first test

Board photos show N16R8, V1695 and an onboard slot. Initial mapping is inferred from the matching Freenove-style layout and manufacturer documentation: 1-bit SDMMC CMD=38, CLK=39, D0=40. The user confirmed this mapping mounts the approximately 62.5 GB destination card. No extra slot wires are required.

The pictured card is a 64 GB Gigastone microSDXC. This build supports FAT16/FAT32 **and exFAT** through the project-local FatFs override. A 64 GB card can retain its exFAT format; capacity itself does not establish the filesystem. Firmware never formats either card. If mounting fails, inspect the card on a PC and preserve any existing files before considering reformatting; do not treat every mount failure as a filesystem problem.

1. When a good card is available, keep camera/source breakout power off for the initial local-card test. Power ESP32 off, insert destination microSD, then power ESP32 through UART and run SDMOUNT. Local SD is no longer mounted automatically at boot.
2. Look for `Local SD mounted /sd`, card information and capacity. Run `SDSTATUS` and `SDSCAN`.
3. Once local mount is confirmed, establish USB source access using the experimentally working standby/USB-power sequence in TEST_RESULTS.md. Avoid motion during a copy.
4. Start with one smaller existing source file, using its exact listed path, for example:

```text
COPY /usb/cell_std/100MEDIA/DSCF0001.JPG
```

5. Follow each POWER ACTION prompt, acknowledging after physically changing external +5 V. Expect RECEIVE STORED first; after final POWER OFF, local processing produces COPY VERIFIED. Repeat in default FAST mode: expect RECEIVE FAST SKIP and no old media payload reads.
6. `SDSCAN` lists local copies. Use `COPYALL` to recursively archive MP4/JPEG files from /usb. Other file extensions are ignored. Empty directories are not archived. Traversal is bounded to 16 directory levels and 767-byte generated paths; failures abort the batch explicitly.

## Copy behavior

COPY/COPYALL, COLLECT COPY and enabled SOAK COPY use the same receive/release/process workflow. Mounting alone never copies. The USB phase enumerates names, sizes and modification times, skips known entries using small local indexes, and streams new/changed files once into the ESP SD inbox. CRC32 and SHA-256 are calculated during that stream. Source size/mtime are checked again, and payload/receipt writes are flushed and synced before release. New content precedes known recovery or optional audit work.

After USB storage is released and final external OFF is confirmed/commanded, the ESP performs local readback, embedded timestamp extraction, archive deduplication, normalization and SD/HD reconciliation. Existing filename/HD queue rules remain unchanged. Source files are never deleted. No transport is attached. See MEDIA_ARCHIVE.md for recovery limitations and commands.

FAST is the default after every boot. It trusts previously recorded path/size/mtime plus local record integrity and destination presence/size; it does not detect same-size corruption or a source replacement with the same fingerprint. COPYMODE VERIFY requests full source reads and local verification. A one-time pass establishes indexes for older archives. PROCESS retries pending local receipts without USB access; disconnect external +5 V before using it manually.

Work runs sequentially in the main task; console commands pause during file operations. USB workers continue separately. Camera activity can still interrupt source access, so this minimizes the access window without proving exclusive ownership or filesystem consistency. Completed receipts survive interruptions; incomplete stages and unindexed orphan payloads are retained, and source files remain untouched.

## Source write protection

ESP-IDF hardcodes FF_FS_READONLY=0. The linker wraps **ff_disk_write**, not its preprocessor alias disk_write. Firmware allows writes only if the drive matches the currently mounted local SD card via `ff_diskio_get_pdrv_card`; all other/unknown drives return RES_WRPRT. USB remains read-only regardless of which volume mounts first. No raw USB write, format or delete API is called. Local writes are restricted by application code to /sd/stonelink; source files use rb.

The original 0.1.0 guard rejected every drive write. Version 0.2.0 deliberately permits the local-card drive through the real FatFS disk-write dispatcher. Repeat binary call-path checks after dependency/linker changes. Protecting this firmware's source writes does not control the trail camera's SD writes.

## Commands

| Command | Behavior |
| --- | --- |
| HELP / STATUS | USB state and command list |
| DEVICE | Stable factory-MAC ESP prefix |
| HD SHOW / REQUEST filename | Inspect timestamp/type group or queue its current HD using an archived filename |
| HD LIST / NEXT / CANCEL request_id / RETRY request_id | Persistent queue controls; NEXT verifies readiness without transmitting |
| META /usb/path | Inspect JPEG/MP4 camera time without copying; mounted source required |
| SCAN | List USB source |
| READ /usb/path | CRC32/ISO-HDLC; no path selects first discovered JPEG |
| WATCH ON / OFF | Status/serialized sector-zero probe; does not control mount retries |
| RESET | MSC BOT recovery; known to produce canceled-transfer errors on this adapter |
| RETRY | Fresh USB acquisition window with up to three attempts; no VBUS switching |
| REINIT | Release source mount/device and reboot ESP32; local SD mount remains manual |
| SDMOUNT | Retry local-card mount; never formats |
| SDSTATUS | Local mount state and total/free filesystem bytes |
| SDSCAN | List local card |
| COPY /usb/path | Verified archive of a regular source file or subtree |
| COPYALL | Verified archive of MP4/JPG/JPEG source files; ignores other extensions |
| COLLECT PLAN / COPY | Power-cycle workflow followed by metadata scan or verified archive, then release |
| POWER STATUS / MODE MANUAL / MODE GPIO | Inspect workflow or explicitly select human/GPIO21 power control |
| POWER OFF / ON / CANCEL | Acknowledge manual prompts or abort the workflow |

References: [Freenove SDMMC pin mapping](https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html), [MSC 1.1.4](https://components.espressif.com/components/espressif/usb_host_msc/versions/1.1.4/readme), [ESP-IDF 5.4 FatFS configuration](https://github.com/espressif/esp-idf/blob/v5.4/components/fatfs/src/ffconf.h). See HARDWARE.md, TEST_RESULTS.md and ROADMAP.md for wiring, observed evidence and remaining work.

## Continue testing without a destination card (0.3.0)

Local SD is skipped at boot. All source writes remain blocked while no local destination is mounted. PLAN and SCHEDULE never invoke COPY/COPYALL or any source write/delete/format API.

After camera standby, manually enable USB 5 V as in the working experimental sequence. Then run:

```text
PLAN
PLAN
```

First PLAN establishes a RAM baseline; second should report unchanged files if source is unchanged. Each PLAN releases/reinstalls the MSC device and mounts FAT fresh, scans media files under DCIM, cell_std and cell_hd, then unmounts/releases source storage again. USB 5 V stays physically on. PLAN does not run the automatic first-JPEG read. STATUS may therefore show installed=0/mounted=0 after a successful plan; retry_paused=1 is intentional. READ/SCAN afterward require RETRY to remount or a new USB attachment. Always reacquire after camera activity; refreshing a mount is not proof the camera has released ownership.

Output classifies BASELINE/NEW/CHANGED/UNCHANGED/MISSING and totals media bytes and cell_std bytes. File IDs hash path/size/FAT timestamp; they are metadata hints, not content checksums or proof a local copy exists. Source content changes that preserve size/time may escape PLAN; COPY still checks actual bytes. Maximum 128 media entries and 255-byte source paths; overflows/errors abort without replacing the preceding baseline. Two snapshots require up to approximately 70 KiB heap, plus traversal and USB runtime memory. PLAN RESET clears only the RAM baseline. REINIT loses it too. Missing files are reported, never deleted.

To test one new capture: establish PLAN baseline, trigger one event and move away, wait for recording to finish, manually cycle USB 5 V after camera returns to standby, then PLAN. This compares a fresh source view without local SD. Do not claim new-photo/video retrieval until READ of that specific new file completes.

### Schedule (dry-run only)

```text
SCHEDULE STATUS
SCHEDULE EVERY 60
SCHEDULE ON
```

This opt-in test runs PLAN every 60 seconds after each completed window. SCHEDULE OFF stops future attempts (current synchronous file/USB operation must finish before console responds). SCHEDULE NOW runs one plan immediately, even if schedule is off. For the eventual six-hour cadence use SCHEDULE EVERY 21600, then ON. Default is six hours and OFF at every boot. Intervals are 30..86400 seconds. Timing is boot uptime, not wall-clock appointment times, and is not persisted; the next interval begins after completion/exhaustion rather than guaranteeing four calendar-time runs daily.

Scheduled failure retries after 30 seconds, at most three plan attempts per window, then waits a full interval. Source acquisition in normal diagnostics is separately limited to three consecutive failures, with 15/30-second backoff, then paused until RETRY or a new USB attachment. Neither loop switches VBUS or promises recovery from the experimentally observed camera-induced stalls. Scheduled plans release the source and pause unrelated background acquisitions. WATCH OFF controls only WATCH reports/probes, not scheduling or acquisition retry policy.

Local destination copying, durable indexes, actual six-hour collection and deletion remain future validation/work. The dry-run planner establishes discovery and timing behavior now without a replacement card.

## Host tests

`tests/collection_test.c` compiles the production planner with fake read-only directory/USB-availability and logging/task adapters. It covers metadata additions/changes/removals, unchanged scans, I/O/unavailable-source rollback, snapshot overflow preservation, exact schedule boundaries, capped retries, disabled NOW behavior and interval validation. It does not simulate electrical behavior or verify FatFS on real media.

On this machine a portable TinyCC compiler was downloaded from the upstream release into ignored .pio/host-tools solely for testing:

```powershell
& '.\.pio\host-tools\tcc\tcc.exe' -Itests/host_stubs -Isrc -run tests/collection_test.c
```

Alternatively, a host GCC can compile with the same include paths and output under .pio. The compiler download is not a firmware dependency and is not tracked in Git.

## FAT32 and exFAT together (0.3.1)

The pinned ESP-IDF FatFs component is overridden by components/fatfs, enabling exFAT while retaining FAT32. See components/fatfs/STONELINK.md for provenance, two configuration changes, licenses and host-test commands. The shared PlatformIO installation is untouched. Compile-time checks enforce exFAT and long-filename support. A disabled volume-label option now explicitly evaluates to zero, avoiding an upstream exFAT compile error.

Upload 0.3.1, power ESP32 off before inserting the exFAT destination, power back on, then SDMOUNT and SDSTATUS. Mount remains manual and never formats. Confirm reported capacity/free space before a small COPY test, then repeat to verify COPY SKIP. FAT32 source and exFAT destination can coexist. FAT32 can also hold more than 32 GB total: Windows formatting-tool limits are distinct from filesystem volume limits. This update targets aggregate card storage, not a guarantee of individual files exceeding the ESP-IDF VFS/off_t limit; source adapter files fit within its approximately 504 MB storage.

Host regression tests now exercise the exact vendored ff.c/ffunicode.c with the production ffconf.h on sparse simulated FAT32 and 64 GiB exFAT disks: format, mount, long-name write, sync, unmount/remount, readback, free-space/geometry and 64-bit FatFs file-size type. No physical disks are accessed by those tests. Device pinout, actual card mount, real SD writes and interruption testing still need bench verification. Existing PLAN/SCHEDULE behavior and source write barrier are retained.

## Overnight test (0.4.0)

See [OVERNIGHT.md](OVERNIGHT.md) for the complete upload, local-copy preflight, logger and shutdown procedure. SOAK ON runs five-minute metadata cycles; SOAK COPY runs verified batches on a mounted FAT32/exFAT destination (default six hours). SOAK EVERY 300 sets a five-minute stress-test interval, SOAK NOW runs immediately, SOAK STATUS reports health/counters, SOAK OFF stops future cycles and persists OFF. ON/COPY starts in 30 seconds; enabling either resets interval to its mode default, so set EVERY afterward. OFF commands wait for any synchronous operation to finish. Mode and interval persist in NVS across ESP resets; diagnostic counters/baseline do not.

Full UART recording uses tools/overnight_log.py with PlatformIO Python; it timestamps every line, handles reconnects and records JSONL/summary under logs/. Keep Windows awake and close other monitors. Optional local /sd/stonelink/soak.csv records structured health/cycle events only. No firmware logging hook writes recursively into FatFS. Automatic VBUS switching, deletion, Wi-Fi and LoRa remain absent. Persistent post-motion USB stalls may prevent subsequent overnight reads until a manual power cycle; failed windows are logged at bounded intervals instead of reported as successful collections.
