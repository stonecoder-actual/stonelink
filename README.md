# StoneLink media collector

ESP32-S3 USB host experiment for the SPYPOINT CL-CBL adapter, with local microSD archiving. Firmware 0.2.0 adds manual verified copying; USB source deletion, scheduling, radio transmission and GPIO-controlled VBUS switching are not implemented.

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

Board photos show N16R8, V1695 and an onboard slot. Initial mapping is inferred from the matching Freenove-style layout and manufacturer documentation: 1-bit SDMMC CMD=38, CLK=39, D0=40. Mount success on this exact board still requires hardware confirmation. No extra slot wires are required.

The pictured card is a 64 GB Gigastone microSDXC. This build supports FAT16/FAT32, **not exFAT** (ESP-IDF 5.4 hardcodes FF_FS_EXFAT=0). A 64 GB card may currently use exFAT; capacity itself does not establish its filesystem. Firmware never formats either card. If mounting fails, inspect the card on a PC and preserve any existing files before considering reformatting; do not treat every mount failure as a filesystem problem.

1. Keep camera/source breakout power off for the initial local-card test. Power ESP32 off, insert destination microSD, then power ESP32 through UART.
2. Look for `Local SD mounted /sd`, card information and capacity. Run `SDSTATUS` and `SDSCAN`.
3. Once local mount is confirmed, establish USB source access using the experimentally working standby/USB-power sequence in TEST_RESULTS.md. Avoid motion during a copy.
4. Start with one smaller existing source file, using its exact listed path, for example:

```text
COPY /usb/cell_std/100MEDIA/DSCF0001.JPG
```

5. Expect `COPY VERIFIED` with source, destination, bytes and CRC32. Run the same command again: expect `COPY SKIP verified` after checksumming both source and existing archive.
6. `SDSCAN` lists local copies. Later use `COPYALL` to recursively archive regular files from /usb, including videos and camera metadata. Empty directories are not archived. Traversal is bounded to 16 directory levels and 767-byte generated paths; failures abort the batch explicitly.

## Copy behavior

Only explicit COPY/COPYALL commands create archive files. Nothing copies automatically on mount. Files live under `/sd/stonelink/<CRC32>_<bytes>/<source path below /usb>`. This records source path/length/checksum and keeps different contents if the camera reuses names. CRC32 is a diagnostic checksum, not a cryptographic content identifier. Existing archive files that fail verification are preserved and reported, not overwritten.

A new file is first checksummed from source, then read again into reserved `/sd/stonelink/.incoming.part`. Stream checksum and source size/mtime must remain consistent. Destination is flushed/fsynced/closed, reopened and fully checksummed, renamed to its archive path, then reopened/checked again. Only then is COPY VERIFIED logged. On failure the batch stops and the USB mount is released; the next copy overwrites the reserved partial staging file. Completed files from earlier in the batch remain. FAT power-loss recovery is not guaranteed: after reboot existing archive files are reverified before skipping. This is why source deletion remains disabled.

Copy/hash operations run sequentially in the main task; USB library/MSC workers continue separately. WATCH and console responses pause during long copies. New large files require two source reads plus destination verification, so allow more time than READ alone. A camera wake can interrupt copying; source read errors, disconnects and checksum/length changes abort the operation. None of this guarantees a consistent shared filesystem while the camera is writing.

## Source write protection

ESP-IDF hardcodes FF_FS_READONLY=0. The linker wraps **ff_disk_write**, not its preprocessor alias disk_write. Firmware allows writes only if the drive matches the currently mounted local SD card via `ff_diskio_get_pdrv_card`; all other/unknown drives return RES_WRPRT. USB remains read-only regardless of which volume mounts first. No raw USB write, format or delete API is called. Local writes are restricted by application code to /sd/stonelink; source files use rb.

The original 0.1.0 guard rejected every drive write. Version 0.2.0 deliberately permits the local-card drive through the real FatFS disk-write dispatcher. Repeat binary call-path checks after dependency/linker changes. Protecting this firmware's source writes does not control the trail camera's SD writes.

## Commands

| Command | Behavior |
| --- | --- |
| HELP / STATUS | USB state and command list |
| SCAN | List USB source |
| READ /usb/path | CRC32/ISO-HDLC; no path selects first discovered JPEG |
| WATCH ON / OFF | Status/serialized sector-zero probe; does not control mount retries |
| RESET | MSC BOT recovery; known to produce canceled-transfer errors on this adapter |
| REINIT | Release source mount/device and reboot ESP32; remounts local SD on startup |
| SDMOUNT | Retry local-card mount; never formats |
| SDSTATUS | Local mount state and total/free filesystem bytes |
| SDSCAN | List local card |
| COPY /usb/path | Verified archive of a regular source file or subtree |
| COPYALL | Verified archive of all regular source files |

References: [Freenove SDMMC pin mapping](https://docs.freenove.com/projects/fnk0086/en/latest/fnk0086/codes/tutorial/4_Read_and_Write_the_SDcard.html), [MSC 1.1.4](https://components.espressif.com/components/espressif/usb_host_msc/versions/1.1.4/readme), [ESP-IDF 5.4 FatFS configuration](https://github.com/espressif/esp-idf/blob/v5.4/components/fatfs/src/ffconf.h). See HARDWARE.md, TEST_RESULTS.md and ROADMAP.md for wiring, observed evidence and remaining work.
