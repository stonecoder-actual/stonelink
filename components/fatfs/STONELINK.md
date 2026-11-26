# Project-local ESP-IDF FatFs override

Copied from PlatformIO framework-espidf **3.50400.0 / ESP-IDF 5.4.0**, component
`fatfs`. Includes the component's production C code, headers, Kconfig and image
generator support; upstream test/example directories are omitted. Upstream
copyright/license notices are preserved; see LICENSE.ESP-IDF and source headers.
The shared PlatformIO framework installation is not modified.

Local modifications in `src/ffconf.h`:

1. FF_FS_EXFAT=1, adding exFAT while retaining FAT12/16/32.
2. FF_USE_LABEL explicitly defaults to zero when its Kconfig boolean is absent.
   Upstream exFAT code uses this macro in an ordinary C expression, which otherwise
   fails to compile with label support disabled.

Existing project CONFIG_FATFS_LFN_HEAP enables long filenames. FF_LBA64 stays zero:
32-bit sector addresses suffice for this 64 GB card; enabling it would change
driver ABI unnecessarily. This does not promise support for every partition
scheme or individual files over the ESP-IDF VFS/off_t limit. Current source
adapter capacity is approximately 504 MB, so archive source files are smaller.

Project `components/fatfs` overrides the framework component through ESP-IDF's
normal component discovery. All component users must rebuild against this header
because exFAT changes FatFs structure layouts. local_storage.c has a compile-time
assertion for exFAT and long-filename support. Retain the pinned framework or audit
and refresh this override when upgrading ESP-IDF.

Host tests use production ff.c/ffunicode.c and this exact ffconf.h, with sparse
simulated disks, host-only sdkconfig and synchronization adapters:

```powershell
& '.\.pio\host-tools\tcc\tcc.exe' -Itests/fatfs_stubs -Itests/host_stubs -Icomponents/fatfs/src -o .pio/fatfs_test.exe tests/fatfs_test.c components/fatfs/src/ff.c components/fatfs/src/ffunicode.c
& .pio/fatfs_test.exe
```

These tests format RAM-backed media only, then write/sync/remount/read and check
geometry on FAT32 and 64 GiB exFAT. They never open physical Windows disks.
