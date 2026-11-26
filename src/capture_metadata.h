#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char datetime[20]; // Camera calendar fields: YYYY-MM-DD HH:MM:SS, no timezone.
    const char *source; // exif_original / exif_digitized / mp4_movie / mp4_track / missing
} capture_metadata_t;
typedef bool (*capture_read_fn)(void *context, uint64_t offset, void *data, size_t length);

// Bounded random-access metadata reader, no allocation or media decoding.
// QuickTime's epoch is decoded into calendar fields without timezone adjustment.
bool capture_metadata_read(capture_read_fn read, void *context, uint64_t size,
                           bool jpeg, capture_metadata_t *out);
void capture_filename_time(const capture_metadata_t *metadata, char out[24]);
bool capture_datetime_valid(const char *text);
