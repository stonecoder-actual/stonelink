#pragma once
#include <stdbool.h>
#include <stdint.h>

bool local_storage_mount(void);
void local_storage_status(void);
void local_storage_scan(void);
void local_storage_identity(void);
void local_storage_metadata(const char *source,bool (*available)(void));
void local_storage_hd(const char *args);
bool local_storage_write_allowed(uint8_t drive);
// USB phase: durable receipts/payloads only; no local readback or normalization.
bool local_storage_receive(const char *source, bool (*available)(void));
// Caller releases USB ownership / completes external OFF before automatic run.
bool local_storage_process_inbox(void);
void local_storage_copy_mode(const char *args);
// Optional local structured journal; raw UART capture remains the full log.
void local_storage_journal(const char *event, uint32_t cycle, const char *result, const char *detail);
