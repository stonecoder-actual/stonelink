# Roadmap

1. Complete: native ESP-IDF USB host and pinned MSC driver compile; linked FatFS write barrier verified statically.
2. Outside-camera enumeration, MSC readiness, camera-created JPEG listings, a complete 2,705,661-byte original JPEG read (CRC32 c65327a4), and one disconnect/reconnect cycle confirmed by user serial logs. Original repeat-read checksum matches; cell_std complete read also confirmed (73,274 bytes, CRC32 de2a112d). cell_hd complete read confirmed (534,575 bytes, CRC32 f5ac82e2); original checksum matches across four reported reads. Independent integrity and recovery-command checks remain pending.
3. REINIT reboot/remount/original READ confirmed with matching CRC32. WATCH supplied probes pass across a 32-second span and OFF is acknowledged (middle log not supplied). RESET reproducibly reports canceled transfers, followed by successful original READ; investigate cause. Repeated-attachment resource stability remains pending.
4. With SD breakout: independently verify pad numbering, power paths and SD/USB ownership.
5. After electrical/ownership evaluation: test camera-installed access and sleep arbitration. This is the feasibility gate.
6. Add controlled VBUS switching only with suitable hardware.
7. Future work after feasibility: local storage, LoRa thumbnails, Wi-Fi full images.

Original receiver repair and blind proprietary commands are out of scope.







