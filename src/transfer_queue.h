#pragma once
#include "archive_naming.h"
typedef struct {
    char prefix[17],capture_id[65],request_id[65],sha256[65],path[768],requested_filename[160];
    uint64_t bytes;
    uint32_t sequence;
} transfer_job_t;
bool transfer_job_json(char *out,size_t capacity,const transfer_job_t *job);
bool transfer_job_parse(const char *json,transfer_job_t *job,const char *prefix,const char *request_id);
typedef bool (*transfer_resolve_fn)(const char *reference,transfer_job_t *job);
typedef bool (*transfer_verify_fn)(const transfer_job_t *job);
void transfer_queue_command(const char *args,const char *folder,const char *prefix,
                            transfer_resolve_fn resolve,transfer_verify_fn verify);
