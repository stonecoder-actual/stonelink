#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef bool (*collection_ready_fn)(void);
// All functions run in the main task; snapshots are RAM-only metadata hints.
bool collection_plan(collection_ready_fn available);
void collection_reset(void);
void collection_status(void);
// Schedule requests PLAN only; main.c owns the external-power workflow.
bool collection_command(const char *args, int64_t now_ms);
bool collection_due(int64_t now_ms);
void collection_finished(bool success, int64_t now_ms);
