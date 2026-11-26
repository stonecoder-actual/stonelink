#include "media_group.h"
#include "media_filter.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

#define PATH_SIZE 768
#define MAX_ENTRIES 10000
static const char *TAG="media_group";
static bool child_path(char *out,const char *folder,const char *name) {
    int n=snprintf(out,PATH_SIZE,"%s/%s",folder,name); return n>=0 && n<PATH_SIZE;
}
static bool move_flag(const char *from,const char *to) {
    if(!strcmp(from,to)) return true;
    struct stat info;
    if(!stat(to,&info) || errno!=ENOENT) {
        ESP_LOGE(TAG,"Flag destination exists/unreadable; no overwrite: %s",to); return false;
    }
    if(rename(from,to)) { ESP_LOGE(TAG,"Flag rename failed errno=%d; retry reconciliation",errno); return false; }
    ESP_LOGI(TAG,"MEDIA FLAG from=%s to=%s",from,to); return true;
}
static bool scan(const char *folder,const char *prefix,media_group_member_fn member,void *context,
                 media_group_result_t *result,char *maximum) {
    memset(result,0,sizeof(*result)); result->known_time=true; maximum[0]=0;
    DIR *dir=opendir(folder);
    if(!dir) return errno==ENOENT;
    bool ok=true; unsigned entries=0; char group_time[20]="",group_kind[6]="";
    for(;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if(!entry) { if(errno) ok=false; break; }
        if(++entries>MAX_ENTRIES) { ok=false; break; }
        if(entry->d_name[0]=='.' || !media_file_supported(entry->d_name)) continue;
        archive_asset_t asset; char path[PATH_SIZE],other[PATH_SIZE]; struct stat info,alias;
        if(!archive_parse_filename(entry->d_name,prefix,&asset) ||
           (member && !member(&asset,context)) || !child_path(path,folder,entry->d_name) ||
           stat(path,&info) || !S_ISREG(info.st_mode) || info.st_size<0) { ok=false; break; }
        if(!result->rank.count) { strcpy(group_time,asset.capture.datetime); strcpy(group_kind,asset.kind); }
        else if(strcmp(group_time,asset.capture.datetime) || strcmp(group_kind,asset.kind)) { ok=false; break; }
        if(!asset.capture.datetime[0]) result->known_time=false;
        if(!archive_quality_path(other,sizeof(other),path,prefix,!strcmp(asset.quality,"SD")?"HD":"SD")) { ok=false; break; }
        if(!stat(other,&alias) || errno!=ENOENT) {
            ESP_LOGE(TAG,"Duplicate/unreadable quality alias for one asset: %s",path); ok=false; break;
        }
        if(!result->rank.count || (uint64_t)info.st_size>result->rank.max_bytes) {
            strcpy(maximum,path); strcpy(result->hd_sha256,asset.sha256);
        }
        archive_rank_add(&result->rank,info.st_size);
    }
    if(closedir(dir)) ok=false;
    if(!ok) ESP_LOGE(TAG,"Group scan incomplete/inconsistent; no classification performed: %s",folder);
    return ok;
}
bool media_group_reconcile(const char *folder,const char *prefix,media_group_member_fn member,
                           void *context,media_group_result_t *result) {
    char maximum[PATH_SIZE];
    if(!scan(folder,prefix,member,context,result,maximum)) return false;
    if(!result->rank.count) return true;
    unsigned moves=0;
    // Restart enumeration after every rename, since FAT directory entries move.
    for(;;) {
        DIR *dir=opendir(folder); if(!dir) return false;
        bool ok=true,found=false; unsigned entries=0; char from[PATH_SIZE],to[PATH_SIZE];
        for(;;) {
            errno=0; struct dirent *entry=readdir(dir);
            if(!entry) { if(errno) ok=false; break; }
            if(++entries>MAX_ENTRIES) { ok=false; break; }
            if(entry->d_name[0]=='.' || !media_file_supported(entry->d_name)) continue;
            archive_asset_t asset; struct stat info;
            if(!archive_parse_filename(entry->d_name,prefix,&asset) || !child_path(from,folder,entry->d_name) || stat(from,&info)) { ok=false; break; }
            if(!strcmp(asset.quality,"HD") && !strcmp(archive_rank_quality(&result->rank,info.st_size,result->known_time),"SD")) {
                found=archive_quality_path(to,sizeof(to),from,prefix,"SD"); if(!found) ok=false; break;
            }
        }
        if(closedir(dir)) ok=false;
        if(!ok) return false;
        if(!found) break;
        if(++moves>result->rank.count || !move_flag(from,to)) return false;
    }
    if(!strcmp(archive_rank_quality(&result->rank,result->rank.max_bytes,result->known_time),"HD")) {
        if(!archive_quality_path(result->hd_path,sizeof(result->hd_path),maximum,prefix,"HD") ||
           !move_flag(maximum,result->hd_path)) return false;
    } else result->hd_sha256[0]=0;
    ESP_LOGI(TAG,"MEDIA GROUP files=%u largest_bytes=%llu largest_ties=%u HD=%d known_time=%d",
             result->rank.count,(unsigned long long)result->rank.max_bytes,result->rank.max_count,result->hd_path[0]!=0,result->known_time);
    return true;
}
bool media_group_find(const char *folder,const char *prefix,const char *sha,char *out,size_t capacity) {
    DIR *dir=opendir(folder); if(!dir) return false;
    bool ok=true,found=false; unsigned entries=0;
    for(;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if(!entry) { if(errno) ok=false; break; }
        if(++entries>MAX_ENTRIES) { ok=false; break; }
        if(entry->d_name[0]=='.' || !media_file_supported(entry->d_name)) continue;
        archive_asset_t asset;
        if(!archive_parse_filename(entry->d_name,prefix,&asset)) { ok=false; break; }
        if(!strcmp(asset.sha256,sha)) {
            if(found) { ok=false; break; }
            int n=snprintf(out,capacity,"%s/%s",folder,entry->d_name);
            if(n<0 || (size_t)n>=capacity) { ok=false; break; }
            found=true;
        }
    }
    if(closedir(dir)) ok=false;
    return ok && found;
}
