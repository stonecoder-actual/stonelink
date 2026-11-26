#pragma once
#include "capture_metadata.h"
#include <stdbool.h>
#include <stdint.h>

// Private on-card record, not a network format. Size/schema/CRC invalidate any
// incompatible firmware layout. Always zero-initialize before sealing/writing.
typedef struct {
    uint32_t magic,schema,record_size,crc32;
    uint64_t bytes;
    int64_t source_mtime;
    char prefix[17],source_path[768],source_key[65],receipt_id[65],sha256[65],capture_id[65];
    char kind[6],datetime[20],time_source[24];
    uint32_t data_crc32;
} ingest_record_t;
bool ingest_record_valid(const ingest_record_t *record,const char *prefix);
void ingest_record_seal(ingest_record_t *record);
bool ingest_source_key(const char *prefix,const char *path,uint64_t bytes,int64_t mtime,char key[65]);
bool ingest_receipt_id(const char *key,const char *sha,unsigned nonce,char id[65]);
bool ingest_layout(const char *root,const char *prefix);
bool ingest_read_record(const char *path,const char *prefix,ingest_record_t *record);
bool ingest_publish_record(const char *path,const ingest_record_t *record,bool asset_identity);
bool ingest_source_lookup(const char *root,const char *prefix,const char *path,uint64_t bytes,int64_t mtime,ingest_record_t *record);
bool ingest_source_publish(const char *root,const ingest_record_t *record);
bool ingest_source_index_path(char *out,size_t capacity,const char *root,const char *prefix,const char *key);
bool ingest_receipt_path(char *out,size_t capacity,const char *root,const char *prefix,const char *id);
bool ingest_payload_path(char *out,size_t capacity,const char *root,const ingest_record_t *record);
bool ingest_asset_path(char *out,size_t capacity,const char *root,const ingest_record_t *record,bool ready);
