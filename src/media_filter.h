#pragma once
#include <stdbool.h>
#include <string.h>
#include <strings.h>

// Shared allowlist for discovery and every source-copy entry point.
static inline bool media_file_supported(const char *path)
{
    const char *ext=strrchr(path,'.');
    return ext && (!strcasecmp(ext,".mp4") || !strcasecmp(ext,".jpg") ||
                   !strcasecmp(ext,".jpeg"));
}
