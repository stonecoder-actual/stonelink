#pragma once
#include <stdint.h>
struct stat { unsigned st_mode; int64_t st_size; int64_t st_mtime; };
#define S_ISDIR(mode) ((mode)==1)
#define S_ISREG(mode) ((mode)==2)
int stat(const char *path, struct stat *info);
