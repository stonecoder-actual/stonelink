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
- No upload or serial hardware test was performed. USB enumeration, filesystem mounting, actual JPEG reads and reconnection remain unverified on hardware.

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
