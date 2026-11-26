#pragma once
struct dirent { char d_name[256]; };
typedef struct { unsigned next; char path[256]; struct dirent entry; } DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
