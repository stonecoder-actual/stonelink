#include "ingest_store.h"
#include "archive_naming.h"
#include "media_io.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static bool ingest_prefix_valid(const char *prefix) {
    if(strlen(prefix)!=16 || strncmp(prefix,"ESP-",4)) return false;
    for(unsigned i=4;i<16;i++) if(!strchr("0123456789ABCDEF",prefix[i])) return false;
    return true;
}
static bool source_valid(const char *path) {
    return !strncmp(path,"/usb/",5) && !strstr(path,"..") && !strchr(path,'\\') && !strchr(path,':') && archive_kind(path);
}
static uint32_t checksum(const ingest_record_t *record) {
    // Do not trust the stored CRC field when calculating integrity.
    size_t offset=offsetof(ingest_record_t,crc32); uint32_t zero=0;
    uint32_t crc=media_crc32_update(UINT32_MAX,(const uint8_t *)record,offset);
    crc=media_crc32_update(crc,(const uint8_t *)&zero,sizeof(zero));
    crc=media_crc32_update(crc,(const uint8_t *)record+offset+sizeof(zero),sizeof(*record)-offset-sizeof(zero));
    return crc^UINT32_MAX;
}
void ingest_record_seal(ingest_record_t *record) {
    record->magic=UINT32_C(0x534c4931); record->schema=1; record->record_size=sizeof(*record);
    record->crc32=checksum(record);
}
bool ingest_record_valid(const ingest_record_t *r,const char *prefix) {
    if(r->magic!=UINT32_C(0x534c4931) || r->schema!=1 || r->record_size!=sizeof(*r) || r->crc32!=checksum(r)) return false;
    if(!memchr(r->prefix,0,sizeof(r->prefix)) || !memchr(r->source_path,0,sizeof(r->source_path)) ||
       !memchr(r->source_key,0,sizeof(r->source_key)) || !memchr(r->receipt_id,0,sizeof(r->receipt_id)) ||
       !memchr(r->sha256,0,sizeof(r->sha256)) || !memchr(r->capture_id,0,sizeof(r->capture_id)) ||
       !memchr(r->kind,0,sizeof(r->kind)) || !memchr(r->datetime,0,sizeof(r->datetime)) ||
       !memchr(r->time_source,0,sizeof(r->time_source))) return false;
    if(!ingest_prefix_valid(prefix) || strcmp(r->prefix,prefix) || !source_valid(r->source_path) ||
       !archive_hex_id(r->source_key) || !archive_hex_id(r->receipt_id) || !archive_hex_id(r->sha256) ||
       (r->capture_id[0] && !archive_hex_id(r->capture_id)) || strcmp(archive_kind(r->source_path),r->kind) ||
       (r->datetime[0] && !capture_datetime_valid(r->datetime))) return false;
    char key[65];
    return ingest_source_key(prefix,r->source_path,r->bytes,r->source_mtime,key) && !strcmp(key,r->source_key);
}
static bool digest_key(const char *text,char out[65]) {
    uint8_t digest[32]; static const char hex[]="0123456789abcdef";
    if(mbedtls_sha256((const unsigned char *)text,strlen(text),digest,0)) return false;
    for(unsigned i=0;i<32;i++) { out[i*2]=hex[digest[i]>>4]; out[i*2+1]=hex[digest[i]&15]; }
    out[64]=0; return true;
}
bool ingest_source_key(const char *prefix,const char *path,uint64_t bytes,int64_t mtime,char key[65]) {
    if(!ingest_prefix_valid(prefix) || !source_valid(path)) return false;
    char text[1024]; int n=snprintf(text,sizeof(text),"source-v1|%s|%s|%" PRIu64 "|%" PRId64,prefix,path,bytes,mtime);
    if(n<0 || n>=sizeof(text)) return false;
    // FAT source names compare without ASCII case; clock value is not converted.
    for(char *p=text;*p;p++) if(*p>='a'&&*p<='z') *p-=32;
    return digest_key(text,key);
}
bool ingest_receipt_id(const char *key,const char *sha,unsigned nonce,char id[65]) {
    if(!archive_hex_id(key) || !archive_hex_id(sha)) return false;
    char text[180]; snprintf(text,sizeof(text),"receipt-v1|%s|%s|%u",key,sha,nonce);
    return digest_key(text,id);
}
static bool directories(char *path) {
    for(char *p=path+1;;p++) {
        if(*p!='/' && *p) continue;
        char saved=*p; *p=0; struct stat info;
        bool ok=!stat(path,&info)?S_ISDIR(info.st_mode):errno==ENOENT && !mkdir(path,0775);
        *p=saved; if(!ok) return false; if(!saved) return true;
    }
}
static bool parent(const char *path) {
    char folder[768]; if(strlen(path)>=sizeof(folder)) return false;
    strcpy(folder,path); char *slash=strrchr(folder,'/'); if(!slash) return false;
    *slash=0; return directories(folder);
}
bool ingest_layout(const char *root,const char *prefix) {
    char path[768];
    if(!ingest_prefix_valid(prefix)) return false;
    int n=snprintf(path,sizeof(path),"%s/%s/inbox",root,prefix);
    return n>=0 && n<sizeof(path) && directories(path);
}
bool ingest_source_index_path(char *out,size_t cap,const char *root,const char *prefix,const char *key) {
    if(!ingest_prefix_valid(prefix) || !archive_hex_id(key)) return false;
    int n=snprintf(out,cap,"%s/%s/source_index/%.2s/%s.idx",root,prefix,key,key); return n>=0 && (size_t)n<cap;
}
bool ingest_receipt_path(char *out,size_t cap,const char *root,const char *prefix,const char *id) {
    if(!ingest_prefix_valid(prefix) || !archive_hex_id(id)) return false;
    int n=snprintf(out,cap,"%s/%s/inbox/%s.receipt",root,prefix,id); return n>=0 && (size_t)n<cap;
}
bool ingest_payload_path(char *out,size_t cap,const char *root,const ingest_record_t *r) {
    if(!ingest_prefix_valid(r->prefix) || !archive_hex_id(r->receipt_id) || (strcmp(r->kind,"PHOTO") && strcmp(r->kind,"VIDEO"))) return false;
    int n=snprintf(out,cap,"%s/%s/inbox/%s.%s",root,r->prefix,r->receipt_id,!strcmp(r->kind,"PHOTO")?"jpg":"mp4");
    return n>=0 && (size_t)n<cap;
}
bool ingest_asset_path(char *out,size_t cap,const char *root,const ingest_record_t *r,bool ready) {
    if(!ingest_prefix_valid(r->prefix) || !archive_hex_id(r->sha256) || (strcmp(r->kind,"PHOTO") && strcmp(r->kind,"VIDEO"))) return false;
    int n=snprintf(out,cap,"%s/%s/asset_index/%.2s/%s_%s.%s",root,r->prefix,r->sha256,r->kind,r->sha256,ready?"ready":"prepared");
    return n>=0 && (size_t)n<cap;
}
bool ingest_read_record(const char *path,const char *prefix,ingest_record_t *record) {
    FILE *file=fopen(path,"rb"); if(!file) return false;
    size_t n=fread(record,1,sizeof(*record),file);
    bool ok=n==sizeof(*record) && fgetc(file)==EOF && !ferror(file);
    if(fclose(file)) ok=false;
    return ok && ingest_record_valid(record,prefix);
}
static bool same_identity(const ingest_record_t *a,const ingest_record_t *b,bool asset) {
    if(strcmp(a->prefix,b->prefix) || strcmp(a->kind,b->kind) || strcmp(a->sha256,b->sha256) || a->bytes!=b->bytes || a->data_crc32!=b->data_crc32) return false;
    return asset ? !strcmp(a->capture_id,b->capture_id) && !strcmp(a->datetime,b->datetime)
                 : !strcmp(a->source_key,b->source_key);
}
bool ingest_publish_record(const char *path,const ingest_record_t *record,bool asset_identity) {
    if(!ingest_record_valid(record,record->prefix) || !parent(path)) return false;
    struct stat info; ingest_record_t check;
    if(!stat(path,&info)) return ingest_read_record(path,record->prefix,&check) && same_identity(record,&check,asset_identity);
    if(errno!=ENOENT) return false;
    char temp[800]; int n=snprintf(temp,sizeof(temp),"%s.part",path); if(n<0 || n>=sizeof(temp)) return false;
    FILE *file=fopen(temp,"wb"); if(!file) return false;
    bool ok=fwrite(record,1,sizeof(*record),file)==sizeof(*record);
    if(fflush(file)) ok=false;
    if(fsync(fileno(file))) ok=false;
    if(fclose(file)) ok=false;
    if(ok) ok=ingest_read_record(temp,record->prefix,&check) && !memcmp(record,&check,sizeof(check));
    return ok && !rename(temp,path);
}
bool ingest_source_lookup(const char *root,const char *prefix,const char *path,uint64_t bytes,int64_t mtime,ingest_record_t *record) {
    char key[65],index[768],conflict[800];
    if(!ingest_source_key(prefix,path,bytes,mtime,key) || !ingest_source_index_path(index,sizeof(index),root,prefix,key)) return false;
    snprintf(conflict,sizeof(conflict),"%s.conflict",index); struct stat info;
    if(!stat(conflict,&info) || errno!=ENOENT) return false;
    return ingest_read_record(index,prefix,record) && !strcmp(record->source_key,key) &&
        !strcasecmp(record->source_path,path) && record->bytes==bytes && record->source_mtime==mtime;
}
bool ingest_source_publish(const char *root,const ingest_record_t *record) {
    char path[768];
    if(!ingest_source_index_path(path,sizeof(path),root,record->prefix,record->source_key)) return false;
    if(ingest_publish_record(path,record,false)) return true;
    // A collision/corrupt index disables metadata-only skipping for this exact
    // fingerprint. Preserve the receipt/index; do not overwrite old evidence.
    char conflict[800]; snprintf(conflict,sizeof(conflict),"%s.conflict",path);
    FILE *file=fopen(conflict,"wb"); if(!file) return false;
    bool ok=fputs("full verification required\n",file)>=0;
    if(fflush(file)) ok=false;
    if(fsync(fileno(file))) ok=false;
    if(fclose(file)) ok=false;
    return ok;
}
