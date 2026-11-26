# Camera-independent filename tracking — firmware 0.7.0

This supersedes 0.6.0's original/folder-based variant naming and JSON media
sidecars. Filenames now carry the media identity, grouping fields and SD/HD flag.
Camera folder names and original camera filenames do not determine pairing.

## Filename and grouping rules

```text
ESP-AABBCCDDEEFF_2026-11-26_14-30-05_PHOTO_SD_<sha256>.jpg
ESP-AABBCCDDEEFF_2026-11-26_14-30-05_PHOTO_HD_<sha256>.jpg
ESP-AABBCCDDEEFF_2026-11-26_14-30-05_VIDEO_SD_<sha256>.mp4
ESP-AABBCCDDEEFF_2026-11-26_14-30-05_VIDEO_HD_<sha256>.mp4
```

Actual filenames contain the full 64-character SHA-256 digest. Prefix is all
six bytes of the factory eFuse base MAC, stable across reboot/reflash/NVS erase.
PHOTO covers JPG/JPEG, with archive extension .jpg; VIDEO covers MP4. Original
source filenames and files are untouched. Only MP4/JPG/JPEG sources are copied.

The timestamp group is exactly `(ESP prefix, camera date/time, PHOTO or VIDEO)`:

- A single distinct file is SD, regardless of absolute size/resolution.
- With multiple distinct files, a uniquely largest file by byte size is HD;
  every smaller file is SD. This also handles three or more versions.
- Equal-size largest ties have no uniquely larger version: all remain SD and
  largest_ties is reported. No arbitrary file is queued as HD.
- Identical bytes are one asset, even if copied from different camera folders.
  Content fingerprints preserve different files at the same timestamp.
- PHOTO and VIDEO at the same second are separate groups. No filename sequence,
  model-specific directory or standard/HD camera-folder convention is assumed.

These are relative file-size labels following the requested rule, not decoded
pixel-resolution measurements. Same-second independent captures of the same
type will share a timestamp group; time/type/size alone cannot prove they are
versions of one sensor event. Camera clock correctness is the user's responsibility.

Internal layout is `/sd/stonelink/<prefix>/captures/<group-id>/<filename>`.
Known-time group ID is SHA-256 of the exact UTF-8/ASCII key
`capture-v3|<prefix>|YYYY-MM-DD HH:MM:SS|PHOTO` (or VIDEO). The future web app can
group directly from filenames; the ID is a directory/queue routing convenience.
No per-media JSON index is required. Queue state is stored separately as JSON.

## Later arrivals and rename recovery

After a verified copy, the ESP rescans that local group and reconciles the flags.
If a new largest version arrives, the former HD becomes SD and the new file
becomes HD. A lone file can also become HD when a smaller matching file arrives.
The fingerprint is unchanged through renames. A client holding the old SD/HD
filename can still identify the asset by its prefix/time/type/fingerprint.

Incorrect HD flags are demoted before the unique largest file is promoted.
Directory enumeration restarts after every rename so moving FAT entries do not
skip other files. No payload is deleted, rewritten or overwritten. On a rename
failure, the cycle fails and media is retained. Next copy, HD SHOW or HD request
reconciles the group again. Duplicate SD/HD aliases for the same fingerprint,
foreign/malformed files inside a group or incomplete scans stop reconciliation.
Group enumeration has an explicit 10,000-entry bound; it does not silently use
a partial inventory. During a copy, earlier COPY lines can name a provisional SD
path; MEDIA FLAG logs subsequent renames. Inspect final names after cycle end.

Legacy CRC-directory and 0.6.0 archives remain in place, with no automatic
migration. New-format copies may duplicate old-format media once. This update
reclassifies only its own new-format groups.

## Camera date policy

JPEG uses EXIF DateTimeOriginal, falling back to DateTimeDigitized. MP4 uses
movie creation time (v0/v1), falling back to track/media creation time. Readers
bound offsets, read counts and recursion and skip compressed payloads. The
camera's encoded calendar values are preserved without timezone/DST conversion
or ESP/laptop time substitution. QuickTime's 1904 epoch is decoded without a
timezone adjustment; compare META output against this camera's clock/overlay.
Current matching precision is whole seconds; EXIF sub-second tags are not parsed.

Missing, invalid or unsupported dates are named UNKNOWN_CAPTURE_TIME. Such
assets are isolated by device/type/content, remain SD and are not automatically
paired or offered as HD. Ordinary filesystem modification time is not silently
called capture time. Actual I/O errors abort reads/copies. META reports what was
found and which metadata source supplied it; no timestamp is read from burned-in
image/video text or assumed from a camera-specific filename.

## HD request queue (local prototype)

After new-format copies finish and local SD is mounted:

```text
HD SHOW <archived-filename-or-capture-id>
HD REQUEST <archived-SD-filename>
HD LIST
HD NEXT
HD CANCEL <request-id>
HD RETRY <request-id>
```

The reference may be just the archived filename or its full local path. SHOW
reconciles the group and lists current filenames. REQUEST parses the same
prefix/time/type fields, verifies the referenced asset exists, selects the
group's uniquely larger HD and verifies its complete SHA-256/size before
enqueueing. A capture ID from CAPTURE/HD logs is also accepted for bench tests.
Missing HD, missing dates or equal-size largest ties produce HD UNAVAILABLE;
no pretend enqueue is reported. Default old camera-folder preferences are gone.

Queue files live under `/sd/stonelink/<prefix>/queue`. Request ID is SHA-256 of
`HD-v1|<group-id>|<HD-content-sha256>`, so repeated requests for the same version
are idempotent. Pending jobs pin full filename/path, checksum and bytes, along
with the requested preview filename and a persistent FIFO sequence. Publication
uses a reserved .job.part file, fsync/readback and rename. Cancellation/retry
renames the local request record; archived media is not deleted. No RAM-only
queue or NVS counter is needed to restore requests after reboot.

NEXT verifies the oldest pending job against the current classification and
rehashes its media. It reports READY, not delivery. Missing/corrupt media or a
new largest version blocks the old request rather than silently retargeting it.
It remains pending; cancel its request ID and request the newly classified HD
if desired. Canceled retries retain the original pinned version and sequence.
The current prototype blocks at an invalid oldest request until resolved or
canceled. No radio/Wi-Fi sender, acknowledgment, automatic delivered state or
web endpoint exists yet. No queue operation claims transmission succeeded.

## Copy integrity and next bench test

Source prepass, streaming copy, local staging readback and final archive readback
must agree on size, CRC32 and SHA-256. Existing media is reverified before skip.
Source size/mtime must stay stable. Filenames are parsed strictly before local
classification or queue routing. Source write barrier remains active.

Upload 0.7.0, stop restored SOAK, run DEVICE, SDMOUNT and SDSTATUS. With the source
powered/mounted (RETRY if needed), inspect a real file with META. Then use
COLLECT COPY and POWER_TEST.md's manual supply prompts. Wait for the final OFF
confirmation/cycle result, then SDSCAN or HD SHOW. Verify a known same-time/type
pair becomes SD/HD according to size. Repeat unchanged copying and require
verified skips with no duplicate assets. Request HD using the final SD filename;
run HD LIST/NEXT, reboot, mount local SD and confirm the pending request remains.
Independently play copied media and exercise rename/queue recovery on hardware.
FAT operations are not guaranteed crash-atomic by these software simulations.

## Host validation

- capture_metadata_test.c: production JPEG/MP4 parsing, damaged/truncated input,
  calendar/unknown-time policy, filename roundtrips and camera-independent keys.
- media_group_test.c: production reconcile logic on a fake local directory;
  lone SD, two/three versions, later larger/smaller arrivals, equal-size ties,
  missing dates, type separation, duplicate aliases and failed rename recovery.
- transfer_queue_test.c: production JSON format/parser using ESP-IDF's cJSON;
  persistence roundtrip, pinned HD paths/checksums and malformed-field rejection.
- transfer_queue_io_test.c: production queue I/O on fake files with target/fsync/
  rename/corruption failures; FIFO, duplicates, reboot and cancellation/retry.
  Its hash function is a deterministic test double, not a cryptography test;
  firmware links real mbedTLS SHA-256. No physical disks/radios are used.

Simple tests run with `.pio/host-tools/tcc/tcc.exe -Itests/host_stubs -Isrc -run
tests/media_group_test.c` (or capture_metadata_test.c). Queue tests compile with
the pinned framework's components/json/cJSON/cJSON.c and include directory;
queue I/O tests additionally use -Itests/queue_stubs. Firmware build validates
actual ESP-IDF dependencies. Real camera metadata/SD/radio behavior remains a
hardware acceptance step.
