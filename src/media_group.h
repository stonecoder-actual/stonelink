#pragma once
#include "archive_naming.h"
typedef bool (*media_group_member_fn)(const archive_asset_t *asset,void *context);
typedef struct {
    archive_rank_t rank;
    bool known_time;
    char hd_path[768],hd_sha256[65];
} media_group_result_t;
// Filenames are authoritative. Demote wrong HD flags before promoting a winner.
// No payload deletion, content changes, source access or sidecar dependency.
bool media_group_reconcile(const char *folder,const char *prefix,media_group_member_fn member,
                           void *context,media_group_result_t *result);
bool media_group_find(const char *folder,const char *prefix,const char *sha,char *path,size_t capacity);
