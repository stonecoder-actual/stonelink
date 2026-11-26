#pragma once
#include <stdbool.h>
#include <stdint.h>

bool local_storage_mount(void);
void local_storage_status(void);
void local_storage_scan(void);
bool local_storage_write_allowed(uint8_t drive);
// Source is /usb or a regular file below it; never writes to that volume.
bool local_storage_copy(const char *source, bool (*available)(void));
// Optional local structured journal; raw UART capture remains the full log.
void local_storage_journal(const char *event, uint32_t cycle, const char *result, const char *detail);
