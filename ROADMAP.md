# Roadmap

1. Complete: native ESP-IDF USB host and pinned MSC driver compile; linked FatFS write barrier verified statically.
2. Outside-camera enumeration, MSC readiness, camera-created JPEG listings, a complete 2,705,661-byte original JPEG read (CRC32 c65327a4), and one disconnect/reconnect cycle confirmed by user serial logs. Original repeat-read checksum matches; cell_std complete read also confirmed (73,274 bytes, CRC32 de2a112d). cell_hd complete read confirmed (534,575 bytes, CRC32 f5ac82e2); original checksum matches across four reported reads. Independent integrity and recovery-command checks remain pending.
3. REINIT reboot/remount/original READ confirmed with matching CRC32. WATCH supplied probes pass across a 32-second span and OFF is acknowledged (middle log not supplied). RESET reproducibly reports canceled transfers, followed by successful original READ; investigate cause. Repeated-attachment resource stability remains pending.
4. With SD breakout: independently verify pad numbering, power paths and SD/USB ownership.
5. After electrical/ownership evaluation: test camera-installed access and sleep arbitration. This is the feasibility gate.
6. Add controlled VBUS switching only with suitable hardware.
7. Future work after feasibility: local storage, LoRa thumbnails, Wi-Fi full images.

Original receiver repair and blind proprietary commands are out of scope.








## Latest camera-installed result

Controlled live tests now demonstrate USB reads with adapter retained in an armed camera after standby, and recovery from motion-associated stalls by manually cycling external USB 5 V. Latest logged JPEG read: 2,200,997 bytes, CRC32 2508205a. REINIT alone failed after motion; USB power cycling succeeded. Next verify video/new-capture retrieval and repeated motion/recovery cycles with fresh pre/post listings. Continuous shared filesystem access, reliable autonomous retrieval, electrical isolation and long-duration stability remain unproven. Earlier entries marking all installed-camera access unverified are superseded by these specific controlled results; the reliable product feasibility gate is not yet complete.

New-media retrieval milestone: DSCF0003.MP4 appeared between fresh mounts and was completely read over USB (6,754,937 bytes, CRC32 c83b0587) in the reported retained-card/armed-camera test. Next: repeat checksum, validate playable contents independently, and repeat controlled capture/recovery cycles. Manual USB 5 V switching remains the demonstrated recovery mechanism; automatic VBUS switching awaits suitable hardware.

## Revised collection direction (user authorized)

Target: four collection windows per day, verified copies on onboard destination SD, then selectively clean up source, LoRa previews and Wi-Fi originals. Current 0.2.0 work implements manual SD mount/status/list and verified COPY/COPYALL only. Next bench milestone: actual local SD mount, one small copy, verified skip on repeat, USB interruption during copy and ESP restart/reverification. Then test full batch and name reuse. Implement scheduling and appropriate powered USB acquisition/release after copy reliability. Source deletion requires separately scoped write access and crash/camera-wake ownership tests; not implemented. LoRa preview size/protocol and high-resolution selection mapping remain future work.
