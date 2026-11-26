#pragma once
#include "capture_metadata.h"

void archive_device_prefix(const uint8_t mac[6],char out[17]);
const char *archive_kind(const char *source);
bool archive_capture_key(char *out,size_t capacity,const char *prefix,const char *source,
                         const capture_metadata_t *metadata);
bool archive_media_path(char *out,size_t capacity,const char *root,const char *prefix,
                        const char *source,const capture_metadata_t *metadata,const char sha256[65],
                        const char capture_id[65],const char *quality);
typedef struct {
    capture_metadata_t capture;
    char kind[6],quality[3],sha256[65];
} archive_asset_t;
bool archive_hex_id(const char *id);
bool archive_parse_filename(const char *name,const char *prefix,archive_asset_t *asset);
bool archive_quality_path(char *out,size_t capacity,const char *path,const char *prefix,const char *quality);
typedef struct { unsigned count,max_count; uint64_t max_bytes; } archive_rank_t;
void archive_rank_add(archive_rank_t *rank,uint64_t bytes);
const char *archive_rank_quality(const archive_rank_t *rank,uint64_t bytes,bool known_time);
