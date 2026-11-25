# Test results

## 2026-11-25 verified development results

- Workspace initially contained only .git, with no commits.
- PlatformIO Core 6.2.0; pinned Espressif32 platform 6.10.0.
- Installed ESP-IDF 5.4.0 (framework package 3.50400.0) and Xtensa toolchain 14.2.0+20241119.
- Resolved usb_host_msc 1.1.4; dependencies.lock records component hash 865a651c08d0bf2ce255a369778375e493df588dfb0720c3d97e12bfdcc4c0f9.
- `platformio run`: SUCCESS. Application uses 316,484 bytes flash within the default 1 MiB application partition; 18,424 bytes static RAM. Runtime task stacks, heap and USB allocations are additional.
- Generated configuration confirms 16 MB flash, UART console, 24,576-byte main task stack, and no enabled PSRAM.
- Build artifact: `.pio/build/spypoint_usb_host/firmware.bin`.
- Firmware SHA256: a68974a7ca75a9ab5b4d5814656c4539fe5459985f5017ca0a38101abbd2395c.
- USB-to-UART device detected twice: COM6, USB-Enhanced-SERIAL CH343, VID:PID 1A86:55D3, serial 5CCC047926. ESP32 chip identity was not queried.
- During initial development, no upload or serial hardware test was performed. Subsequent user-supplied serial evidence is recorded below.

## Read-only verification

Installed ESP-IDF ffconf.h hardcodes FF_FS_READONLY=0. It also remaps disk_write to ff_disk_write. Disabling formatting alone is insufficient.

The firmware links `--wrap=ff_disk_write`. Symbol inspection confirms `__wrap_ff_disk_write` exists and the original `ff_disk_write` is absent from the linked image. Disassembly shows 18 calls redirected to the wrapper. Its body logs the attempted write and returns 2 (RES_WRPRT), without calling storage. Disassembly is available in the ignored build directory as `disassembly.txt`.

Pinned MSC source inspection confirms its normal diskio write callback would otherwise issue SCSI WRITE10; its ioctl supports synchronization and geometry queries, with no trim operation. Mounting uses format_if_mount_failed=false; this application calls no raw write or format API. This verifies the firmware's FatFS write barrier statically, not simultaneous camera access or hardware behavior. Repeat verification after dependency or linker changes.

## Recovery implementation (hardware verification pending)

RESET uses standard MSC Bulk-Only Transport recovery, not a physical bus reset or VBUS cycle. REINIT releases the mount/device and restarts the ESP32 to reinitialize USB. WATCH reports state and performs a serialized sector-zero read every two seconds; failed probes release the mount for retry. Attached unmounted storage is retried every five seconds. File operations and raw probes run in one task, while library and MSC event handling continue separately.

The initial configuration attempt failed because ESP-IDF Git version detection could not resolve this repository's unborn HEAD. An explicit PROJECT_VER fixes this without changing Git history.

## Bench acceptance procedure (pending)

1. Follow HARDWARE.md with adapter outside camera and native USB-C disconnected.
2. Upload over UART on COM6, then open the monitor at 115200 using README commands.
3. Capture initialization, descriptors, MSC detection, capacity, FAT mount and listing.
4. READ a JPEG twice and compare CRC32/ISO-HDLC with a PC checksum of the same bytes.
5. Unplug/replug the adapter, including during SCAN/READ; verify recovery and retain error logs.
6. Exercise HELP, STATUS, WATCH ON/OFF, RESET and REINIT. Confirm repeated reconnections without exhausting heap/resources.
7. Record actual serial logs here. Prior observations in HARDWARE.md are user-reported experiments; illustrative handoff logs are not evidence.

## 2026-11-25 user-supplied startup serial log

Confirmed from serial output supplied by the user:

- ESP32-S3 chip revision v0.2 boots the stonelink application, version 0.1.0, on ESP-IDF 5.4.0.
- Bootloader reports 16 MB flash; application starts successfully.
- At 330 ms: `stonelink: USB host initialized; waiting for device; type HELP`.
- No USB device connection, MSC detection, capacity, or mount event appears in the supplied excerpt.

Host initialization passes. Adapter enumeration, filesystem access, JPEG retrieval and reconnection remain pending. This excerpt does not establish whether the adapter was attached or powered during the test.

## 2026-11-25 adapter enumeration and FAT listing (user-supplied serial evidence)

Setup confirmed by user: CL-CBL connected to powered Mini-USB breakout, outside camera. Initial lack of detection was due to breakout power being off.

Passed in supplied log:

- USB enumeration: VID 05e3, PID 0761, USB 2.00; one interface, class 08, subclass 06, protocol 50 (MSC/SCSI transparent/Bulk-Only Transport).
- Endpoints: bulk IN 0x81, bulk OUT 0x02, 64-byte maximum packets.
- At 61,940 ms, mass-storage interface detected and MSC attached at address 1.
- At 62,880 ms, storage ready: 985,087 sectors x 512 bytes = 504,364,544 bytes reported by firmware.
- FAT mounted with write barrier enabled. FF_FS_READONLY=0 is expected for this framework; the verified linker barrier supplies write rejection.
- Directory scan completed: System Volume Information; DCIM/100MEDIA; cell_std/100MEDIA; cell_hd/100MEDIA. No regular files/JPEGs were listed.
- JPEG retrieval/checksum remains untested: firmware reported No JPEG discovered.

Startup sense response: key 06, ASC 28, ASCQ 00. T10 maps this to unit attention for a not-ready-to-ready transition / medium may have changed (https://t10.org/lists/asc-num.htm). Subsequent readiness, mounting and scanning succeeded, so this response did not prevent this test. Do not infer persistent media failure from this single recovered response.

Capacity differs from the earlier user-reported 504,102,912-byte measurement by 261,632 bytes. A whole-device versus filesystem/partition measurement is a possible explanation, not established. Preserve both measurements pending verification.

Next: place a known JPEG on the adapter through a PC SD reader while disconnected from the ESP32 setup, safely eject, reconnect outside camera, and verify READ checksum. Then test removal/reconnection. Camera-installed access remains untested.

## 2026-11-25 after camera formatting (user-supplied evidence)

User reports adapter no longer appears on laptop, but camera still accepts it and successfully formats it. Laptop connection method and device/volume visibility are not yet established; do not attribute this to the laptop or adapter without further evidence.

Subsequent ESP32 serial log confirms host initialization at 330 ms, USB connection at 730 ms, VID:PID 05e3:0761, MSC detection at 810 ms, readiness and FAT mount at 1750 ms. Reported capacity remains 985,087 x 512 = 504,364,544 bytes. The same startup sense response 06/28/00 occurs before successful mount.

Scan completes at 1750 ms without listing any directory entries, followed by No JPEG discovered at 1760 ms. This establishes a successful mount and a scan yielding no entries, not successful JPEG retrieval. Camera formatting may explain an empty filesystem; no post-format photograph is evidenced by this log. Next test: create and confirm a new photo in the camera with Mini-USB disconnected, then power camera off, remove adapter and repeat ESP32 scan outside camera. If camera playback confirms a photo but ESP32 still lists none, investigate the discrepancy rather than assuming storage is shared identically.

## 2026-11-25 camera-created JPEG visible over USB (user-supplied serial log)

User reports sequential transfer: disconnected from ESP32, inserted into camera, powered camera on, took picture, removed adapter and reconnected to ESP32. Camera shutdown before removal and playback confirmation were not explicitly reported.

Evidence:

- Address 3 mounted with an empty scan at 158700 ms.
- USB device removed and MSC disconnected at 195070 ms.
- Reattached at address 4 at 236870 ms; storage ready and FAT mounted at 237880 ms, same capacity as earlier tests.
- /usb/DCIM/100MEDIA/DSCF0001.JPG: 2,705,661 bytes.
- /usb/cell_std/100MEDIA/DSCF0001.JPG: 73,274 bytes.
- /usb/cell_hd/100MEDIA/DSCF0001.JPG: 534,575 bytes.
- Scan completed at 237970 ms. No READ completion or CRC32 line was included.

Confirmed: camera-created photo entries become visible through ESP32 USB access after physically transferring the adapter, and a disconnect/reconnect cycle recovers sufficiently to mount and list files. Multiple same-named JPEGs of differing sizes exist; thumbnail/reduced-resolution roles are plausible but image dimensions and contents have not been inspected. Complete JPEG byte retrieval/integrity, resource stability across many cycles, and camera-installed USB access remain unverified.

Next: wait for automatic first-JPEG READ completion, or issue READ /usb/cell_std/100MEDIA/DSCF0001.JPG, followed by the original DCIM file. Record byte counts and CRC32 values and repeat to check stability.

## 2026-11-25 complete JPEG read confirmed (user-supplied serial evidence)

After enumeration and successful FAT mount at 5700 ms, listing reports /usb/DCIM/100MEDIA/DSCF0001.JPG with size 2,705,661 bytes. Scan completes at 5790 ms. At 19000 ms firmware reports:

`READ /usb/DCIM/100MEDIA/DSCF0001.JPG bytes=2705661 CRC32/ISO-HDLC=c65327a4`

Confirmed: a complete byte read of the camera-created original JPEG through ESP32 USB MSC, matching its listed file length, with CRC32 calculated. Approximately 13.21 seconds elapsed from scan completion to read completion (about 205 kB/s including application delays and filesystem overhead; not a raw USB benchmark). No independent PC checksum or image decode has yet verified contents; repeated-read stability also remains pending.

This achieves the outside-camera listing and complete photo-read milestone. It does not establish USB access with the adapter installed in the camera. Next: repeat the same READ and compare byte count/CRC32, then read both smaller same-named JPEGs. Keep the sequential, outside-camera setup for these tests.

## 2026-11-25 repeat original and smaller JPEG reads (user-supplied evidence)

- At 139470 ms: READ /usb/DCIM/100MEDIA/DSCF0001.JPG bytes=2705661 CRC32/ISO-HDLC=c65327a4. Both byte count and checksum match the prior complete original read, confirming repeat-read consistency for this file in this setup.
- At 139840 ms: READ /usb/cell_std/100MEDIA/DSCF0001.JPG bytes=73274 CRC32/ISO-HDLC=de2a112d. Complete read matches the previously listed smaller file size. Its image dimensions/content and repeat checksum have not yet been independently verified.

Remaining bench checks: cell_hd JPEG read; repeated reads of smaller variants; independent PC checksum/image verification; RESET, REINIT, WATCH and repeated reconnection/resource stability. Camera-installed USB access and safe electrical/filesystem ownership remain unverified.

## 2026-11-25 all three JPEG variants read (user-supplied evidence)

Consolidated log confirms original /usb/DCIM/100MEDIA/DSCF0001.JPG reads at 19000, 113030, 126250 and 139470 ms, each returning 2,705,661 bytes and CRC32 c65327a4. These are four reported reads of the same original, including previously recorded results (not four additional reads).

| File | Complete bytes read | CRC32/ISO-HDLC |
| --- | ---: | --- |
| /usb/DCIM/100MEDIA/DSCF0001.JPG | 2705661 | c65327a4 |
| /usb/cell_std/100MEDIA/DSCF0001.JPG | 73274 | de2a112d |
| /usb/cell_hd/100MEDIA/DSCF0001.JPG | 534575 | f5ac82e2 |

cell_hd read completes at 212770 ms, matching its listed size. All three files have now been read completely through ESP32 USB MSC outside the camera. Original-file repeated reads are consistent. No image decode, independent PC checksum comparison, repeated checksum for the smaller files, long-duration resource test, or camera-installed access is established by this evidence.

Next bench checks: STATUS, RESET followed by another original READ, then REINIT and successful remount/automatic original READ; compare with baseline c65327a4. WATCH and additional reconnection tests can follow. Maintain outside-camera wiring until power backfeeding and filesystem ownership are evaluated.

## 2026-11-25 recovery-command test (user-supplied attached log)

- Windows serial monitor reports GetOverlappedResult PermissionError/access denied, then reconnects to COM6. Cause not established; this is the laptop/UART connection, distinct from the adapter's native USB MSC connection.
- One unknown command appears, followed by successful STATUS: address=1, installed=1, mounted=1, removed=0, WATCH=0.
- Two Transfer failed: Status 3 messages occur five seconds apart, with BOT, REQUEST SENSE and unit-ready errors. Installed ESP-IDF usb_types_stack.h defines transfer status 3 as CANCELED. These errors are compatible with the reported recovery-test sequence, but command echo is absent, so exact command attribution is not established. Do not classify RESET as error-free.
- A subsequent full original READ succeeds: 2,705,661 bytes, CRC32 c65327a4.
- REINIT is explicitly logged at 2271210 ms, followed by RTC_SW_CPU_RST. After reboot, host initializes at 335 ms, re-enumerates adapter at 735 ms, mounts FAT at 1745 ms, lists all three JPEGs, and completes the original READ at 15045 ms with the same byte count and CRC32 c65327a4.

Confirmed: REINIT reboot/remount/automatic read works in this test, and file reading succeeds following the earlier transfer errors. RESET requires a separately observed/echoed test to establish its exact outcome and explain canceled transfers. No evidence of photo-content changes appears in the repeated original checksum. Long-duration reliability and installed-camera access remain unverified.

## 2026-11-25 isolated RESET errors reproduced (user-supplied serial output)

Following the requested RESET-only test, user supplied Transfer failed: Status 3 at 267485 ms and 272485 ms, with bot_execute_command, scsi_cmd_sense and scsi_cmd_unit_ready errors. This reproduces the two cancellations five seconds apart seen in the earlier recovery sequence. Status 3 maps to CANCELED in installed ESP-IDF. Exact low-level cause is not yet established; do not infer a wiring fault or permanent media failure from these messages.

RESET is not validated as a clean recovery operation on this adapter. Post-RESET READ from this isolated test is still pending. Earlier combined test did produce a matching original READ after similar errors; REINIT is separately confirmed to reboot/remount/read successfully. Prefer REINIT for deliberate recovery until RESET behavior is investigated.

## 2026-11-25 read after isolated RESET errors confirmed

User-supplied output at 322375 ms: READ /usb/DCIM/100MEDIA/DSCF0001.JPG bytes=2705661 CRC32/ISO-HDLC=c65327a4. This matches the established original-file baseline after the isolated RESET test produced two canceled transfers.

Confirmed: file access remains functional after this RESET error sequence; no content change is detected by this checksum. RESET remains noisy/unexplained and is not validated as clean recovery. REINIT remains the demonstrated reboot/remount/read alternative. Next useful outside-camera check is WATCH ON for approximately 30 seconds, then WATCH OFF; record whether sector-zero probes stay readable or show errors.

## 2026-11-25 initial WATCH monitoring (user-supplied serial evidence)

WATCH enabled at log timestamp 363835 ms. Six sector-zero probes through 373935 ms all report media_readable=1; each state report remains address=1 installed=1 mounted=1 removed=0 WATCH=1. Observed interval is 10.100 seconds, not yet a complete 30-second test. No transfer errors or detachments appear in this excerpt. The duplicate initial STATUS is consistent with the command's immediate status report plus the periodic WATCH report; it is not a second device attachment.

Confirmed: six successful serialized read-only availability probes in this interval outside the camera. Longer monitoring, clean WATCH OFF, and repeated smaller-file checks remain pending; this does not establish camera-installed access or long-term reliability.

## 2026-11-25 WATCH interval completed and disabled

User supplied final WATCH probe at 396155 ms: address=1 installed=1 mounted=1 removed=0 WATCH=1; media_readable=1. At 396595 ms STATUS confirms WATCH=0 with storage still mounted. From first supplied probe (363835 ms) to final supplied probe is 32.320 seconds. Initial six probes and final probe all succeeded; intermediate output between 373935 and 396155 ms was not supplied, so absence of errors during that gap is not independently established. WATCH OFF was accepted; no subsequent log was provided to independently observe continued silence.

Outside-camera baseline now includes complete reads of all three JPEGs, consistent original CRC32 across repeated reads, one confirmed reconnect, REINIT reboot/remount/read, and successful sampled WATCH probes across a 32-second span with disable acknowledged. RESET cancellations remain unresolved. Camera-installed electrical power paths and filesystem ownership remain the next feasibility investigation, not a validated configuration.

## 2026-11-25 storage unavailable, setup not yet confirmed

User supplied a new log showing repeated sense key 02, ASC 3A, ASCQ 00 (not ready / medium not present), then MSC readiness/install ESP_FAIL at 582305 ms. Later retries show canceled transfers (Status 3), NO SENSE responses and MSC internal error 0x1703 through 657725 ms. No successful mount or file read appears in this excerpt. NO SENSE after failed transfers must not be interpreted as successful readiness.

Physical setup, camera state, external VBUS connection and initiating action have not been reported for this log. Camera ownership/arbitration is a possible explanation only if the adapter was installed in the camera; power and transport problems are also unexcluded. Do not classify this as confirmed SD-priority behavior or damaged storage.

User also supplied a voltage of 2.765 V, but camera state and exact measurement setup remain unconfirmed. Do not combine this measurement with the new log as if simultaneous conditions were established. Next action: clarify adapter location, camera mode/power, external breakout 5 V state and the change immediately preceding errors; restore previously validated outside-camera baseline as appropriate.

## 2026-11-25 camera on/off with external VBUS lead physically disconnected

User confirms this test series uses external 5 V wire disconnected from breadboard, adapter installed in camera and USB data/GND connected to ESP32; WATCH enabled. Camera mode and exact switch-action timestamps were not annotated in the supplied log.

- Initial STATUS at 1145335 ms: no device, installed=0 mounted=0 removed=1 WATCH=1.
- USB enumeration at 1155215 ms, address 5, same VID/PID and MSC interface.
- Repeated sense 02/3A/00 (not ready / medium not present) starts at 1155315 ms; install fails at 1160435 ms.
- STATUS while connected: address=5 installed=0 mounted=0 removed=0. Later automatic install retries produce canceled transfers and 0x1703 errors; no FAT mount or media_readable=1 occurs.
- At 1190435 ms transfer status 7 (NO_DEVICE in installed ESP-IDF enum) appears with invalid-state errors during an in-flight install attempt. USB removal/MSC disconnect follows at 1190495 ms, approximately 35.28 seconds after enumeration.
- Subsequent STATUS: address=0 installed=0 mounted=0 removed=1; WATCH disabled at 1200195 ms.

Interpretation: USB identification works in the reported camera-powered configuration but usable storage is not obtained. This failure recurs with the external supply lead physically absent, so an attached unpowered breadboard supply is not a necessary cause; other power limitations and storage arbitration remain unresolved. Removal sequence is consistent with the reported camera-off action, but exact correspondence requires user timing confirmation. WATCH reports state only when installed=0: its raw sector probes require a successfully installed MSC handle, so lack of media_readable lines here is expected. Retry error bursts arise from MSC install readiness/transport handling, not successful WATCH reads. This approximately 35-second attachment does not test long camera-idle/sleep behavior.

## 2026-11-25 longer camera-connected WATCH log, address 6

Supplied 418-line log starts at 1430935 ms with no USB device. Enumeration occurs at 1435775 ms (address 6, VID:PID 05e3:0761). MSC readiness initially reports 02/3A/00; later install retries repeatedly report canceled transfers and 0x1703. No successful install, FAT mount, file read, or media_readable=1 appears anywhere in the supplied log.

Device removal appears at 1618445 ms, 182670 ms (3 minutes 2.67 seconds) after enumeration. A NO_DEVICE transfer and invalid-state errors precede removal. WATCH OFF acknowledged at 1625845 ms. This is not evidence of five minutes after screen-off; camera mode, screen-off time, and whether final removal corresponds to manual camera power-off remain unconfirmed.

Interpretation: no storage access was achieved during the observed attachment. Repeated failed install/transport recovery may itself affect ability to detect later readiness, so do not conclude camera never releases storage or that the product is infeasible from this run. External VBUS lead remains disconnected per user's declared test-series setup. Need distinguish deliberate camera-off from automatic USB disappearance before designing the next test. Earlier 2.765 V reading still lacks confirmed camera-state attribution.

## 2026-11-25 planned camera-first then external USB 5 V test: successful storage transition

User supplied log after proposing ESP32 on, adapter camera-installed, camera on first and USB-breakout 5 V enabled afterward. The log contains no action markers, so actual camera/card state and exact 5 V switching time at the successful transition require confirmation.

- Address 26 connects at 2768915 ms. Initial sense 02/3A/00 and readiness failure are followed by repeated canceled transfers / internal errors; no storage mount at that address.
- Removal occurs at 2844235 ms. Address 27 connects at 2845895 ms (1.660 seconds later).
- At 2846895 ms storage is ready and FAT mounts, reporting the same 504,364,544-byte geometry.
- Lists DSCF0001.JPG through DSCF0007.JPG in each of DCIM, cell_std and cell_hd (21 JPEG entries). The original DSCF0007.JPG is 2,546,954 bytes; smaller counterparts are 41,291 and 447,430 bytes.
- At 2860395 ms complete original DSCF0001.JPG READ succeeds: 2,705,661 bytes, CRC32 c65327a4, matching baseline.
- Six WATCH probes through 2870485 ms succeed; WATCH OFF acknowledged at 2871605 ms with storage still mounted.

Confirmed from log alone: USB disconnect/re-enumeration was followed by successful filesystem mount, listing and known-file read, plus successful availability probes. If the user confirms the adapter stayed inserted in the powered camera and only external USB 5 V changed, this would establish a first camera-installed read under that specific power sequence. Do not yet mark that feasibility gate achieved or attribute the transition solely to USB power: camera power-off, physical card removal, and other actions are not timestamped. A successful read also would not establish safe simultaneous camera writes or electrical isolation.

### Clarification of preceding camera-first / external USB power test

User reports camera behaved normally; after turning it on and waiting without apparent storage success, user turned camera off. Thus the later successful mount/read must not be claimed as camera-on installed access. The log does show eventual successful mount and original-file READ after the failed address-26 session and address-27 re-enumeration. Exact timing of external 5 V relative to camera-off, and whether card remained physically installed throughout the successful read, still require confirmation. User's impression of no connection after camera-off is contradicted by the eventual mount/read in the supplied log, though timing of the physical action is not marked.

### Final physical-sequence clarification: success was outside camera

User explicitly confirms the successful address-27 mount/read occurred only after unplugging the adapter from the camera and cycling breakout USB 5 V off/on. Supersedes any earlier suggestion that success occurred camera-installed or merely after camera-off.

Observed outcome: no successful camera-installed filesystem access in this test, including the user's camera-off waiting period; exact external 5 V enable time during the installed portion remains unmarked. Removal from camera plus USB power cycle restored the established outside-camera baseline and original CRC32 c65327a4. These two changes were made together, so the log cannot distinguish which change was necessary for recovery. No camera problems were reported by user, but this is not verification of electrical isolation or simultaneous-write safety. Camera-installed feasibility gate remains unmet; SD arbitration, power behavior and firmware/transport recovery remain possible contributors.

## Methodical live test series: USB-powered adapter retained in camera

Test 1: user reports mounted and read successfully with adapter physically inserted in switched-off camera, external USB 5 V on, and fresh ESP32 host via REINIT. This establishes camera-off installed access for that power sequence; no detailed log supplied for this step.

Test 2: user supplies WATCH log after instruction to turn only camera power on, retaining USB 5 V and all connections. Eight probes from 74075 through 88215 ms succeed. At 90235 ms transfer status 4 (STALL) occurs; canceled transfer follows and WATCH reports media_readable=0 at 95245 ms. Subsequent install attempts stall/fail; STATUS retains address=1 removed=0 but installed=0 mounted=0. No USB detach appears in the supplied excerpt through 125505 ms. Exact camera-on timestamp and camera-side card errors were not supplied.

This demonstrates loss of previously working USB storage access during the reported camera-startup test while external USB 5 V remains on. Simple lack of external VBUS is therefore not sufficient to explain all camera-installed failures. Storage ownership switching and power/signal interactions remain hypotheses; USB transport recovery is also in a failed state. Next controlled variable: camera off only, retaining adapter insertion, external USB 5 V, ESP32 power and WATCH; no RESET/REINIT or supply cycle yet. Observe whether automatic retries restore readiness.

### Live Test 3: camera off only, no automatic recovery observed

In response to camera-off-only instructions, user supplies continuing alternating stalled transfers (Status 4) and canceled transfers (Status 3), SCSI inquiry/sense failures and MSC internal 0x1703. STATUS through 216195 ms remains address=1 installed=0 mounted=0 removed=0 WATCH=1. Supplied interval 170825-216225 ms spans 45.4 seconds; camera-off action itself is not timestamped. No mount, read or successful probe occurs.

Conclusion for this sequence: camera-off alone did not visibly restore storage through existing retries. This does not distinguish adapter state from host/transport state. Next Test 4: keep camera off, adapter inserted and external USB 5 V continuously on; issue REINIT only and observe re-enumeration/mount/read. Camera-side card-error question remains unanswered.

### Live Test 4: REINIT without adapter power cycle failed to restore storage

Supplied log confirms ESP32 software reset and host initialization at 335 ms, USB enumeration at 735 ms and MSC attach at 825 ms. First SCSI inquiry stalls at 835 ms; canceled transfer/readiness failure at 5835 ms. Later attempts produce general transfer errors (Status 1) and ESP_ERR_INVALID_STATE through at least 45935 ms. No storage ready/mount/read appears. Per controlled-test instructions, camera remains off, adapter remains inserted, external USB 5 V remains continuously on; user supplied no change to those conditions.

Conclusion: rebooting the ESP32/reinitializing native USB alone did not clear this camera-startup-induced failure in this trial. Next Test 5: camera off, card retained, ESP32 left running; cycle only external USB 5 V off for ten seconds then on. Observe whether USB removal occurs during off interval and readiness/mount/read returns. If no USB removal, this is not a demonstrated complete adapter power cycle and camera-off/data-line residual power remains possible.

### Live Test 5: external USB power cycle restored camera-off installed access

Following instructions to retain adapter inside switched-off camera and leave ESP32 running, user supplies USB removal at 84015 ms, reconnect at 97835 ms (13.82-second event gap), FAT mount at 98845 ms, listing of seven JPEGs in each of three directories, and complete original READ at 112335 ms: 2,705,661 bytes, CRC32 c65327a4.

Confirmed for this controlled sequence: adapter removal from camera was unnecessary for recovery; cycling external USB 5 V restored file access while card remained inserted and camera was off. REINIT alone had failed in preceding test. Does not prove all adapter circuitry lost power during off interval, that USB cycling is universally necessary, or that camera-on access is possible.

Next controlled startup-order test: with adapter retained, switch USB 5 V off and observe disconnect; turn camera on with USB 5 V off, allow startup to settle, then enable USB 5 V while camera stays on. Observe whether fresh USB-side startup acquires storage while camera active. No RESET, REINIT or card removal during that trial. This differs from Test 2, which started camera after a USB mount was already active.

### Live Test 6: camera-first startup order did not obtain storage

Per test instructions adapter remains camera-installed, camera on first, then external USB 5 V enabled while camera remains on; ESP32 running. Log enumerates address 3 at 182775 ms, detects MSC at 182855 ms, and repeatedly receives 02/3A/00 (medium not present) through 220245 ms. Final supplied readiness/install failure is ESP_FAIL at 220345 ms. No successful mount/read appears. This startup order does not obtain camera-on storage in this trial. Unlike the earlier USB-first trial's stalled endpoints, this excerpt repeatedly receives structured not-ready sense replies, so it is useful to test camera-off-only once from this different transport state before any power cycling.

Next: change only camera to off, retain insertion and external USB 5 V, wait 20 seconds for mount/read or continuing errors. This distinguishes recovery from the clean medium-not-present state from recovery after USB-first stalled transfers. Neither startup order has yet demonstrated camera-on USB file access. Results support SD-side state/ownership involvement but do not prove a specific mechanism or exclude power interactions.

Correction after inspecting the untruncated event summary: Test 6 also contains general transfer error/invalid-state at 195185-195215 ms, USB removal at 195235 ms and a new address-4 connection at 211775 ms. Address 4 again reports medium not present and fails readiness at 220345 ms. The log is not one continuous clean medium-not-present session. Physical actions causing the intervening detach/reconnect were not marked; clarify whether USB power, camera power or cable state changed before interpreting this test further. Supersedes the preceding comparison implying no transport error occurred in this excerpt.

### Full-battery repeat of camera-first startup order

User reports camera battery fully charged, then supplies repeat-test log. REINIT is explicit, followed by host init at 335 ms, USB enumeration at 10855 ms (address 1), MSC detection at 10935 ms, readiness/install ESP_FAIL at 16065 ms, and repeated canceled transfers/internal 0x1703 through 76395 ms. No mount/read or post-reboot USB removal/root-port-reset-failed event appears in supplied excerpt. Initial sense reports 02/3A/00, followed later by NO SENSE after failed transfers.

Conclusion: charging the camera did not eliminate storage-access failure in this repeat; low battery alone is insufficient to explain the results. The earlier transient detach/root-port-reset sequence did not recur in this supplied interval. Exact external 5 V enable time is not marked. Camera-derived VBUS can enumerate USB before external 5 V is enabled, so initial MSC failure could precede full external power; host/transport state then remains a confound.

Next single-change test: keep camera on, card inserted, external 5 V on and stable; issue REINIT only, then observe whether newly initialized host under stable dual-power conditions mounts/reads. This differs from earlier REINIT test with camera off and from enumerating before external USB 5 V was enabled.

### Full-battery camera-on REINIT with external USB 5 V already enabled

User supplies log following instruction to retain camera on, adapter inserted and external USB 5 V on, changing only REINIT. Software reset and host initialization succeed; adapter enumerates at 735 ms and MSC attaches at 825 ms. Initial readiness returns 02/3A/00 starting at 835 ms; install fails at 5945 ms. Subsequent canceled transfers/internal 0x1703 appear at 15965/15975 and 20995/21005 ms, continuing through supplied final line at 26025 ms. No successful storage mount/read.

Conclusion: initializing a fresh ESP32 host after both camera and external USB supply are already on does not obtain storage in this trial. Initial enumeration before external 5 V alone is insufficient to explain failure. Camera-off USB-powered installed access and recovery by camera-off USB cycle remain the demonstrated working sequence. Camera-on SD/USB arbitration remains a strong hypothesis, not proof of mechanism; camera mode/sleep state, actual VBUS during dual-power operation and driver recovery limitations remain unresolved. Next establish whether camera is in setup/screen-active state or armed/screen-off state before extending tests.

## Armed-standby observations and controlled external-5V-off startup

User previously reported a mount and complete read after camera announced ready for motion detection, screen turned off, and REINIT was run; camera reported no SD-card problem. No supporting mount/READ lines were supplied for that occurrence, and user then reported access stopped again without establishing whether a motion event preceded loss. Treat as user-reported armed-state success pending repeatable logged confirmation, not sustained-access verification.

New supplied startup log, external USB 5 V off: adapter enumerates at 49695 ms, MSC attaches at 49785 ms, reports 02/3A/00 and fails install at 54905 ms; subsequent retries fail. NO_DEVICE at 88895 ms precedes USB removal/MSC disconnect at 88945 ms, 39.250 seconds after enumeration. User explicitly associates this disconnect with camera entering standby, not manual camera power-off. Thus this differs from earlier deliberate-off disconnects.

Confirmed: USB presence disappeared at the user-observed standby transition with external USB power off. A camera-supplied power/USB-signaling change is plausible; removal alone does not identify the internal cause or prove storage is released. Next single variable: leave camera armed and card inserted, enable external USB 5 V only, no REINIT initially; observe automatic enumeration/mount/read for 30 seconds while avoiding motion triggers. This tests whether fresh externally powered USB access becomes available in armed standby.

### Armed-standby test: enabling external USB 5 V obtains mount/read

Following the observed standby USB disconnect with external 5 V off, user reports mounted and read after enabling only external USB 5 V, keeping adapter installed and camera armed with screen off. No REINIT was requested for this transition. This is a user-reported successful armed-state mount/complete read under a defined sequence, pending supplied READ byte-count/CRC line and repeatability testing.

Working sequence reported: camera starts with external USB 5 V off; camera enters armed standby and USB disappears; enabling external USB 5 V then obtains mount/read. This distinguishes standby timing from simply enabling USB power during camera startup. Power/signal gating and storage ownership mechanisms remain inferred, not established. Next: WATCH for 30 seconds without deliberate motion, retaining all conditions, to separate idle stability from capture-triggered behavior. Only after stable idle observations should one motion event be introduced.

### Armed-idle WATCH stability: 90.890 seconds, all 46 probes successful

User supplies continuous WATCH log from 225815 through 316705 ms while following armed-idle test instructions (camera armed/screen off, adapter retained, external USB 5 V on, no deliberate motion). All 46 media_readable probes report 1. Every STATUS retains address=2 installed=1 mounted=1 removed=0 WATCH=1; no errors or detachments appear. Duplicate initial STATUS is expected from command plus periodic report.

Confirmed: installed/mounted storage and serialized sector-zero reads remain available throughout this approximately 91-second armed-idle sample. The excerpt contains no READ completion/CRC line; prior armed-state complete read remains user-reported rather than directly evidenced by supplied read log. Successful sector-zero probes alone do not prove a new camera photo was saved or guarantee filesystem consistency during writes.

Next single-variable experiment: retain camera armed, external USB 5 V and WATCH; trigger one motion event, move outside detection area, observe for 30 seconds without resets or power changes. Record first error or persistent successful probes; subsequently verify whether camera actually captured a new photo before drawing capture-arbitration conclusions. Avoid SCAN of a potentially stale live mount until remount strategy is chosen from observed behavior.
