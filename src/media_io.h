#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { int64_t last_yield_us; unsigned yields; } media_io_guard_t;
// Raw CRC state convention retained: start UINT32_MAX, xor UINT32_MAX at end.
uint32_t media_crc32_update(uint32_t raw,const uint8_t *data,size_t length);
void media_io_begin(media_io_guard_t *guard);
void media_io_checkpoint(media_io_guard_t *guard);
