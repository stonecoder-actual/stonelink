# StoneLink USB diagnostic

ESP32-S3 USB host experiment for the SPYPOINT CL-CBL adapter. Scope: enumeration and read-only FAT file retrieval. No radio, camera capture, vendor commands, or VBUS control.

## Build and run

Use PlatformIO Core (`C:\Users\Sam\.platformio\penv\Scripts\platformio.exe` on this machine):

```
$pio = 'C:\Users\Sam\.platformio\penv\Scripts\platformio.exe'
& $pio run
& $pio run -t upload --upload-port COM6
& $pio device monitor --port COM6 --baud 115200
```

COM6 was detected as USB-Enhanced-SERIAL CH343, VID:PID 1A86:55D3, serial 5CCC047926. Confirm this identity if Windows assigns a different port. Upload is a separate user action; no hardware success is implied by a build.

Dependencies: Espressif32 6.10.0, ESP-IDF 5.4.0 (PlatformIO package 3.50400.0), usb_host_msc 1.1.4. See dependency lock after resolution. PSRAM is intentionally unused until the exact board wiring is verified. Flash is configured for 16 MB; verify module marking before upload.

See HARDWARE.md, TEST_RESULTS.md, and ROADMAP.md.

## Read-only enforcement

ESP-IDF 5.4 hardcodes `FF_FS_READONLY=0`; disabling formatting alone does not enforce read-only access. This project therefore links `--wrap=ff_disk_write`: every FatFS sector write is rejected with `RES_WRPRT` before it reaches MSC. Application files are opened only with `rb`, formatting is disabled, and no raw write API is called. This protects this firmware's FatFS path, not another host such as the camera. Verify the final link and pinned component call path after dependency updates.

Console: HELP, STATUS, SCAN, READ /usb/path/to/file.jpg (without path uses first discovered JPEG), RESET (standard MSC BOT recovery, not physical bus reset), REINIT (release mount/device, reboot host), WATCH [ON|OFF] (two-second state reports; automatic mount retry runs independently). No VBUS switching occurs.

Reference implementations: [Espressif MSC component 1.1.4](https://components.espressif.com/components/espressif/usb_host_msc/versions/1.1.4/readme), [ESP-IDF 5.4 USB host API](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/peripherals/usb_host.html), [ESP-IDF 5.4 FatFS configuration](https://github.com/espressif/esp-idf/blob/v5.4/components/fatfs/src/ffconf.h).
WATCH also probes sector zero with a serialized read-only MSC read; a failed probe releases the mount so the automatic retry can reacquire storage. It does not establish camera ownership or filesystem consistency.

Build verified on 2026-11-25. Firmware output: `.pio/build/spypoint_usb_host/firmware.bin`. UART upload and bench verification remain pending. The default application partition is 1 MiB even though physical flash is configured for 16 MB. Directory traversal is bounded to 16 levels and paths to 511 bytes; skipped entries are logged. CRC32 uses the standard ISO-HDLC parameters (same result as Python zlib.crc32). See TEST_RESULTS.md for build sizes, binary hash, and linked write-barrier evidence.
