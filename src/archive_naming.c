#include "archive_naming.h"
#include "media_filter.h"
#include <stdio.h>

void archive_device_prefix(const uint8_t mac[6],char out[17]) {
    snprintf(out,17,"ESP-%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
bool archive_hex_id(const char *id) {
    if(!id || strlen(id)!=64) return false;
    for(unsigned i=0;i<64;i++) if(!strchr("0123456789abcdef",id[i])) return false;
    return true;
}
static bool prefix_valid(const char *prefix) {
    if(strlen(prefix)!=16 || strncmp(prefix,"ESP-",4)) return false;
    for(unsigned i=4;i<16;i++) if(!strchr("0123456789ABCDEF",prefix[i])) return false;
    return true;
}
const char *archive_kind(const char *source) {
    if(!media_file_supported(source)) return NULL;
    return !strcasecmp(strrchr(source,'.'),".mp4")?"VIDEO":"PHOTO";
}
bool archive_capture_key(char *out,size_t capacity,const char *prefix,const char *source,
                         const capture_metadata_t *metadata) {
    const char *kind=archive_kind(source);
    if(!kind || !capture_datetime_valid(metadata->datetime) || !prefix_valid(prefix)) return false;
    // Deliberately independent of camera folders, original names and model.
    int n=snprintf(out,capacity,"capture-v3|%s|%s|%s",prefix,metadata->datetime,kind);
    return n>=0 && (size_t)n<capacity;
}
static bool filename(char *out,size_t capacity,const char *prefix,const capture_metadata_t *capture,
                     const char *kind,const char *quality,const char *sha) {
    if(!prefix_valid(prefix) || !archive_hex_id(sha) ||
       (strcmp(quality,"SD") && strcmp(quality,"HD")) ||
       (strcmp(kind,"PHOTO") && strcmp(kind,"VIDEO")) ||
       (capture->datetime[0] && !capture_datetime_valid(capture->datetime))) return false;
    char stamp[24]; capture_filename_time(capture,stamp);
    int n=snprintf(out,capacity,"%s_%s_%s_%s_%s.%s",prefix,stamp,kind,quality,sha,
                   !strcmp(kind,"PHOTO")?"jpg":"mp4");
    return n>=0 && (size_t)n<capacity;
}
bool archive_media_path(char *out,size_t capacity,const char *root,const char *prefix,
                        const char *source,const capture_metadata_t *metadata,const char sha[65],
                        const char id[65],const char *quality) {
    const char *kind=archive_kind(source); char leaf[160];
    if(!kind || !archive_hex_id(id) || !filename(leaf,sizeof(leaf),prefix,metadata,kind,quality,sha)) return false;
    int n=snprintf(out,capacity,"%s/%s/captures/%s/%s",root,prefix,id,leaf);
    return n>=0 && (size_t)n<capacity;
}
bool archive_parse_filename(const char *name,const char *prefix,archive_asset_t *asset) {
    memset(asset,0,sizeof(*asset)); asset->capture.source="archive_filename";
    if(!prefix_valid(prefix) || strlen(name)<114 || strncmp(name,prefix,16) || name[16]!='_') return false;
    const char *p=name+17;
    if(!strncmp(p,"UNKNOWN_CAPTURE_TIME_",21)) p+=20;
    else {
        memcpy(asset->capture.datetime,p,19); asset->capture.datetime[19]=0;
        if(p[10]!='_' || p[13]!='-' || p[16]!='-') return false;
        asset->capture.datetime[10]=' '; asset->capture.datetime[13]=asset->capture.datetime[16]=':';
        if(!capture_datetime_valid(asset->capture.datetime)) return false;
        p+=19;
    }
    if(*p++!='_' || (strncmp(p,"PHOTO_",6) && strncmp(p,"VIDEO_",6))) return false;
    memcpy(asset->kind,p,5); p+=6;
    if((strncmp(p,"SD_",3) && strncmp(p,"HD_",3)) || strlen(p)!=71) return false;
    memcpy(asset->quality,p,2); p+=3;
    memcpy(asset->sha256,p,64); p+=64;
    return archive_hex_id(asset->sha256) && !strcmp(p,!strcmp(asset->kind,"PHOTO")?".jpg":".mp4");
}
bool archive_quality_path(char *out,size_t capacity,const char *path,const char *prefix,const char *quality) {
    const char *base=strrchr(path,'/'); if(!base) return false;
    archive_asset_t asset; char leaf[160];
    if(!archive_parse_filename(base+1,prefix,&asset) ||
       !filename(leaf,sizeof(leaf),prefix,&asset.capture,asset.kind,quality,asset.sha256)) return false;
    int n=snprintf(out,capacity,"%.*s/%s",(int)(base-path),path,leaf);
    return n>=0 && (size_t)n<capacity;
}
void archive_rank_add(archive_rank_t *rank,uint64_t bytes) {
    rank->count++;
    if(rank->count==1 || bytes>rank->max_bytes) { rank->max_bytes=bytes; rank->max_count=1; }
    else if(bytes==rank->max_bytes) rank->max_count++;
}
const char *archive_rank_quality(const archive_rank_t *rank,uint64_t bytes,bool known_time) {
    return known_time && rank->count>1 && rank->max_count==1 && bytes==rank->max_bytes?"HD":"SD";
}
