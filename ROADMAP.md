# Roadmap

1. Complete: native ESP-IDF USB host and pinned MSC driver compile; linked FatFS write barrier verified statically.
2. Bench validation outside camera: descriptors, FAT listing, JPEG CRC32, removal/reconnection.
3. Verify reset/reinitialization and repeated attachment without resource leaks.
4. With SD breakout: independently verify pad numbering, power paths and SD/USB ownership.
5. After electrical/ownership evaluation: test camera-installed access and sleep arbitration. This is the feasibility gate.
6. Add controlled VBUS switching only with suitable hardware.
7. Future work after feasibility: local storage, LoRa thumbnails, Wi-Fi full images.

Original receiver repair and blind proprietary commands are out of scope.
