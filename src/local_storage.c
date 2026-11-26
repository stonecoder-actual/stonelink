#include "local_storage.h"
#include "media_filter.h"
#include "archive_naming.h"
#include "media_group.h"
#include "transfer_queue.h"
#include "ingest_store.h"
#include "media_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ff.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

#if !FF_FS_EXFAT || !FF_USE_LFN
#error "StoneLink local storage requires project-local exFAT and long filename support"
#endif

#define PATH_SIZE 768
#define STORE "/sd/stonelink"
#define STAGING STORE "/.incoming.part"
static const char *TAG = "local_sd";
static sdmmc_card_t *card;
static uint8_t buffer[8192]; // All operations run in the main task.
static bool (*source_available)(void);
static bool copy_verify_mode; // FAST by default; explicit VERIFY audits old bytes.
static unsigned received,fast_skipped,staged_skipped,ignored,planned,recovery_planned;
static char device_prefix[17];
static media_group_result_t active_group;

static bool ensure_identity(void)
{
    if(device_prefix[0]) return true;
    uint8_t mac[6];
    esp_err_t err=esp_efuse_mac_get_default(mac);
    if(err!=ESP_OK) { ESP_LOGE(TAG,"Factory MAC read failed: %s; no anonymous archive",esp_err_to_name(err)); return false; }
    unsigned any=0,all=255;
    for(unsigned i=0;i<6;i++) { any|=mac[i]; all&=mac[i]; }
    if(!any || all==255) { ESP_LOGE(TAG,"Invalid factory MAC; no anonymous archive"); return false; }
    archive_device_prefix(mac,device_prefix);
    ESP_LOGI(TAG,"DEVICE prefix=%s identity=factory_base_mac; stable across reboot/flash; camera timezone not converted",device_prefix);
    return true;
}
void local_storage_identity(void)
{
    if(ensure_identity()) ESP_LOGI(TAG,"DEVICE prefix=%s",device_prefix);
}

bool local_storage_write_allowed(uint8_t drive)
{
    // Resolve the actual registered physical drive, never assume SD is drive 0.
    // If unregistered, get_pdrv returns 0xff: no drive is allowed to write.
    return card && drive != 0xff && ff_diskio_get_pdrv_card(card) == drive;
}

bool local_storage_mount(void)
{
    if (card) { local_storage_status(); return true; }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_39;
    slot.cmd = GPIO_NUM_38;
    slot.d0 = GPIO_NUM_40;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 6, .allocation_unit_size = 0,
        .disk_status_check_enable = true,
    };
    sdmmc_card_t *found = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x). No formatting attempted. FAT32/exFAT enabled; check seating, filesystem and board pinout (CMD38 CLK39 D0=40).", esp_err_to_name(err), err);
        return false;
    }
    card = found; // Writes become possible only for this successfully mounted card.
    ESP_LOGI(TAG, "Local SD mounted /sd; physical drive=%u; FAT32/exFAT enabled; CMD38 CLK39 D0=40, 1-bit", ff_diskio_get_pdrv_card(card));
    sdmmc_card_print_info(stdout, card);
    local_storage_status();
    return true;
}

void local_storage_status(void)
{
    if (!card) { ESP_LOGI(TAG, "SDSTATUS mounted=0; insert card with ESP32 powered off, then reboot or SDMOUNT"); return; }
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info("/sd", &total, &free_bytes);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "SDSTATUS mounted=1 drive=%u total=%" PRIu64 " free=%" PRIu64, ff_diskio_get_pdrv_card(card), total, free_bytes);
    else ESP_LOGE(TAG, "SDSTATUS capacity query failed: %s", esp_err_to_name(err));
}

static bool joined(char *out, const char *parent, const char *name)
{
    int n = snprintf(out, PATH_SIZE, "%s/%s", parent, name);
    if (n < 0 || n >= PATH_SIZE) { ESP_LOGE(TAG, "Path too long: %s/%s", parent, name); return false; }
    return true;
}

static bool list_tree(const char *path, unsigned depth)
{
    if (depth > 16) { ESP_LOGE(TAG, "Depth limit at %s", path); return false; }
    DIR *dir = opendir(path);
    if (!dir) { ESP_LOGE(TAG, "opendir %s errno=%d", path, errno); return false; }
    bool ok = true;
    struct dirent *entry;
    for (;;) {
        errno=0; entry=readdir(dir);
        if (!entry) { if (errno) { ESP_LOGE(TAG, "readdir %s errno=%d", path, errno); ok=false; } break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_SIZE]; struct stat st;
        if (!joined(child, path, entry->d_name) || stat(child, &st)) { ok=false; break; }
        ESP_LOGI(TAG, "%s size=%lld", child, (long long)st.st_size);
        if (S_ISDIR(st.st_mode) && !list_tree(child, depth+1)) { ok=false; break; }
    }
    closedir(dir);
    return ok;
}

void local_storage_scan(void)
{
    if (!card) { local_storage_status(); return; }
    ESP_LOGI(TAG, "Local SD scan %s", list_tree("/sd", 0) ? "complete" : "failed");
}

// Never follows source paths outside /usb; all destination paths are generated.
static bool valid_source(const char *path)
{
    return path && (!strcmp(path, "/usb") || !strncmp(path, "/usb/", 5)) &&
           !strstr(path, "..") && !strchr(path, '\\') && !strchr(path, ':');
}

static void sha_hex(const uint8_t digest[32],char text[65])
{
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++) { text[i*2]=hex[digest[i]>>4]; text[i*2+1]=hex[digest[i]&15]; }
    text[64]=0;
}
static bool capture_identity(const char *source,const capture_metadata_t *metadata,const char *sha,
                             char capture_id[65],bool *linkable)
{
    char key[1024]; uint8_t digest[32];
    *linkable=archive_capture_key(key,sizeof(key),device_prefix,source,metadata);
    if(!*linkable) {
        int n=snprintf(key,sizeof(key),"unlinked-v3|%s|%s|%s",device_prefix,archive_kind(source),sha);
        if(n<0 || n>=sizeof(key)) return false;
    }
    if(mbedtls_sha256((const unsigned char *)key,strlen(key),digest,0)) return false;
    sha_hex(digest,capture_id); return true;
}
static bool hash_file(const char *path, bool source, uint64_t *bytes, uint32_t *crc, char sha[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Read open failed %s errno=%d", path, errno); return false; }
    uint64_t count=0; uint32_t value=0xffffffffu; size_t n;
    int64_t started=esp_timer_get_time(); media_io_guard_t guard; media_io_begin(&guard);
    mbedtls_sha256_context context; mbedtls_sha256_init(&context);
    bool ok=mbedtls_sha256_starts(&context,0)==0;
    while (ok && (n=fread(buffer, 1, sizeof(buffer), f))) {
        if (source && !source_available()) { ok=false; break; }
        count+=n; value=media_crc32_update(value, buffer, n);
        if(mbedtls_sha256_update(&context,buffer,n)) { ok=false; break; }
        media_io_checkpoint(&guard);
    }
    if (ferror(f) || (source && !source_available())) ok=false;
    if (fclose(f)) ok=false;
    uint8_t digest[32];
    if(ok && mbedtls_sha256_finish(&context,digest)) ok=false;
    mbedtls_sha256_free(&context);
    if (!ok) { ESP_LOGE(TAG, "Incomplete checksum read %s", path); return false; }
    *bytes=count; *crc=value^0xffffffffu;
    sha_hex(digest,sha);
    ESP_LOGI(TAG,"IO HASH storage=%s bytes=%" PRIu64 " elapsed_ms=%lld yields=%u",source?"USB":"LOCAL",count,
             (long long)((esp_timer_get_time()-started)/1000),guard.yields);
    return true;
}

typedef struct { FILE *file; bool io_error,on_usb; } metadata_file_t;
static bool metadata_read_at(void *context,uint64_t offset,void *out,size_t length)
{
    metadata_file_t *reader=context;
    if(reader->io_error) return false; // One failed USB read ends further metadata I/O.
    if((reader->on_usb && !source_available()) || offset>LONG_MAX || fseek(reader->file,(long)offset,SEEK_SET) ||
       fread(out,1,length,reader->file)!=length) { reader->io_error=true; return false; }
    return true;
}
static bool read_metadata_file(const char *storage_path,const char *source,uint64_t size,bool on_usb,capture_metadata_t *metadata)
{
    FILE *file=fopen(storage_path,"rb");
    if(!file) { ESP_LOGE(TAG,"META open failed %s errno=%d",storage_path,errno); return false; }
    metadata_file_t reader={file,false,on_usb};
    const char *ext=strrchr(source,'.');
    capture_metadata_read(metadata_read_at,&reader,size,strcasecmp(ext,".mp4")!=0,metadata);
    bool ok=!reader.io_error && !ferror(file) && (!on_usb || source_available());
    if(fclose(file)) ok=false;
    if(ok) ESP_LOGI(TAG,"META source=%s camera_datetime=%s time_source=%s timezone=camera_unspecified conversion=NONE storage=%s",
                   source,metadata->datetime[0]?metadata->datetime:"UNKNOWN_CAPTURE_TIME",metadata->source,on_usb?"USB":"LOCAL");
    else ESP_LOGE(TAG,"META read failed; source may be unavailable: %s",source);
    return ok;
}
void local_storage_metadata(const char *source,bool (*available)(void))
{
    if(!valid_source(source) || !media_file_supported(source) || !available || !available()) {
        ESP_LOGE(TAG,"Use META /usb/path/to/file.mp4 (or .jpg/.jpeg) with mounted source"); return;
    }
    struct stat st;
    if(stat(source,&st) || !S_ISREG(st.st_mode)) { ESP_LOGE(TAG,"META requires a regular file"); return; }
    source_available=available;
    capture_metadata_t metadata;
    read_metadata_file(source,source,st.st_size,true,&metadata);
}
static bool mkdirs(char *path)
{
    // Called only on generated paths below STORE. Reject regular-file collisions.
    for (char *p=path+4; ; p++) {
        if (*p != '/' && *p != 0) continue;
        char saved=*p; *p=0;
        struct stat st;
        if (stat(path, &st)==0) {
            if (!S_ISDIR(st.st_mode)) { *p=saved; return false; }
        } else if (mkdir(path, 0775)) { *p=saved; return false; }
        *p=saved;
        if (!saved) break;
    }
    return true;
}

static bool group_folder(char *out,const char *id)
{
    if(!archive_hex_id(id)) return false;
    int n=snprintf(out,PATH_SIZE,STORE "/%s/captures/%s",device_prefix,id);
    return n>=0 && n<PATH_SIZE;
}
static bool group_member(const archive_asset_t *asset,void *context)
{
    char id[65]; bool linkable;
    return capture_identity(!strcmp(asset->kind,"PHOTO")?"media.jpg":"media.mp4",
                            &asset->capture,asset->sha256,id,&linkable) && !strcmp(id,(const char *)context);
}
static bool reconcile_capture(const char *id,const char *sha,char *path)
{
    char folder[PATH_SIZE];
    if(!group_folder(folder,id) || !media_group_reconcile(folder,device_prefix,group_member,(void *)id,&active_group)) return false;
    return !sha || media_group_find(folder,device_prefix,sha,path,PATH_SIZE);
}
typedef enum { CACHE_NEW, CACHE_VERIFIED, CACHE_STAGED, CACHE_RECOVERY } cache_state_t;
static bool same_payload(const ingest_record_t *a,const ingest_record_t *b)
{
    return a->bytes==b->bytes && a->data_crc32==b->data_crc32 && !strcmp(a->kind,b->kind) && !strcmp(a->sha256,b->sha256);
}
static bool asset_record(const ingest_record_t *receipt,bool ready,ingest_record_t *asset)
{
    char path[PATH_SIZE];
    return ingest_asset_path(path,sizeof(path),STORE,receipt,ready) && ingest_read_record(path,device_prefix,asset) &&
           same_payload(receipt,asset) && archive_hex_id(asset->capture_id);
}
static bool asset_file(const ingest_record_t *asset,char *path)
{
    char folder[PATH_SIZE]; struct stat info;
    return group_folder(folder,asset->capture_id) && media_group_find(folder,device_prefix,asset->sha256,path,PATH_SIZE) &&
           !stat(path,&info) && S_ISREG(info.st_mode) && info.st_size>=0 && (uint64_t)info.st_size==asset->bytes;
}
static cache_state_t cached_source(const char *source,const struct stat *info)
{
    ingest_record_t receipt,asset,pending; char path[PATH_SIZE]; struct stat local;
    if(!ingest_source_lookup(STORE,device_prefix,source,info->st_size,info->st_mtime,&receipt)) return CACHE_NEW;
    if(asset_record(&receipt,true,&asset) && asset_file(&asset,path)) return CACHE_VERIFIED;
    if(ingest_receipt_path(path,sizeof(path),STORE,device_prefix,receipt.receipt_id) &&
       ingest_read_record(path,device_prefix,&pending) && same_payload(&receipt,&pending)) {
        if(ingest_payload_path(path,sizeof(path),STORE,&receipt) && !stat(path,&local) && S_ISREG(local.st_mode) &&
           local.st_size>=0 && (uint64_t)local.st_size==receipt.bytes) return CACHE_STAGED;
        if(asset_record(&receipt,false,&asset) && asset_file(&asset,path)) return CACHE_STAGED;
    }
    return CACHE_RECOVERY;
}
void local_storage_copy_mode(const char *args)
{
    if(args && !strcasecmp(args,"FAST")) copy_verify_mode=false;
    else if(args && !strcasecmp(args,"VERIFY")) copy_verify_mode=true;
    else if(args && *args && strcasecmp(args,"STATUS")) { ESP_LOGI(TAG,"COPYMODE FAST | VERIFY | STATUS"); return; }
    ESP_LOGI(TAG,"COPYMODE mode=%s default_after_boot=FAST; indexed metadata skips do not rehash old content",
             copy_verify_mode?"VERIFY":"FAST");
}
typedef struct {
    uint32_t crc32,priority;
    int64_t bytes,mtime;
    char source[PATH_SIZE];
} receive_work_t;
static uint32_t work_crc(const receive_work_t *work)
{
    return media_crc32_update(UINT32_MAX,(const uint8_t *)work+sizeof(work->crc32),sizeof(*work)-sizeof(work->crc32))^UINT32_MAX;
}
static bool discover_sources(const char *path,unsigned depth,FILE *worklist)
{
    if(!source_available() || depth>16) return false;
    struct stat info;
    if(stat(path,&info)) { ESP_LOGE(TAG,"Source discovery stat failed %s errno=%d",path,errno); return false; }
    if(S_ISREG(info.st_mode)) {
        if(!media_file_supported(path)) { ignored++; return true; }
        if(info.st_size<0) return false;
        cache_state_t state=cached_source(path,&info);
        if(!copy_verify_mode && (state==CACHE_VERIFIED || state==CACHE_STAGED)) {
            if(state==CACHE_VERIFIED) fast_skipped++; else staged_skipped++;
            ESP_LOGI(TAG,"RECEIVE %s source=%s bytes=%lld; metadata match, no payload read",
                     state==CACHE_VERIFIED?"FAST SKIP":"ALREADY STAGED",path,(long long)info.st_size);
            return true;
        }
        if(planned>=65535 || strlen(path)>=PATH_SIZE) return false;
        receive_work_t item={0}; strcpy(item.source,path); item.bytes=info.st_size; item.mtime=info.st_mtime;
        item.priority=state==CACHE_NEW?0:1; item.crc32=work_crc(&item);
        if(fwrite(&item,1,sizeof(item),worklist)!=sizeof(item)) return false;
        planned++; if(item.priority) recovery_planned++;
        return true;
    }
    if(!S_ISDIR(info.st_mode)) return false;
    DIR *dir=opendir(path); if(!dir) return false;
    bool ok=true;
    for(;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if(!entry) { if(errno) ok=false; break; }
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[PATH_SIZE];
        if(!joined(child,path,entry->d_name) || !discover_sources(child,depth+1,worklist)) { ok=false; break; }
    }
    if(closedir(dir)) ok=false;
    return ok;
}
static bool unused_receipt(ingest_record_t *record,char *payload,char *receipt)
{
    for(unsigned nonce=0;nonce<65535;nonce++) {
        if(!ingest_receipt_id(record->source_key,record->sha256,nonce,record->receipt_id) ||
           !ingest_payload_path(payload,PATH_SIZE,STORE,record) ||
           !ingest_receipt_path(receipt,PATH_SIZE,STORE,device_prefix,record->receipt_id)) return false;
        struct stat info;
        if(!stat(payload,&info)) continue;
        if(errno!=ENOENT) return false;
        if(!stat(receipt,&info)) continue;
        if(errno!=ENOENT) return false;
        char bad[800]; snprintf(bad,sizeof(bad),"%s.bad",receipt);
        if(!stat(bad,&info)) continue;
        if(errno!=ENOENT) return false;
        return true;
    }
    return false;
}
static bool receive_one(const receive_work_t *item)
{
    struct stat before,after;
    if(!source_available() || stat(item->source,&before) || !S_ISREG(before.st_mode) || before.st_size!=item->bytes || before.st_mtime!=item->mtime) {
        ESP_LOGE(TAG,"Source changed since discovery: %s",item->source); return false;
    }
    ingest_record_t record={0}; strcpy(record.prefix,device_prefix); strcpy(record.source_path,item->source);
    strcpy(record.kind,archive_kind(item->source)); record.bytes=item->bytes; record.source_mtime=item->mtime;
    if(!ingest_source_key(device_prefix,item->source,record.bytes,record.source_mtime,record.source_key)) return false;
    FILE *in=fopen(item->source,"rb");
    if(!in) return false;
    char stage[PATH_SIZE]; snprintf(stage,sizeof(stage),STORE "/%s/inbox/.receiving.part",device_prefix);
    FILE *out=fopen(stage,"wb"); if(!out) { fclose(in); return false; }
    int64_t started=esp_timer_get_time(); media_io_guard_t guard; media_io_begin(&guard);
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha);
    bool ok=mbedtls_sha256_starts(&sha,0)==0; uint64_t total=0; uint32_t crc=UINT32_MAX; size_t n;
    ESP_LOGI(TAG,"RECEIVE begin priority=%s source=%s bytes=%lld",item->priority?"OLD_RECOVERY_OR_AUDIT":"NEW_OR_CHANGED",item->source,(long long)item->bytes);
    while(ok && (n=fread(buffer,1,sizeof(buffer),in))) {
        if(!source_available() || fwrite(buffer,1,n,out)!=n) { ok=false; break; }
        total+=n; crc=media_crc32_update(crc,buffer,n);
        if(mbedtls_sha256_update(&sha,buffer,n)) ok=false;
        media_io_checkpoint(&guard);
    }
    if(ferror(in) || !source_available() || total!=record.bytes || stat(item->source,&after) ||
       after.st_size!=before.st_size || after.st_mtime!=before.st_mtime) ok=false;
    if(fclose(in)) ok=false;
    if(fflush(out)) ok=false;
    if(fsync(fileno(out))) ok=false;
    if(fclose(out)) ok=false;
    uint8_t digest[32];
    if(ok && mbedtls_sha256_finish(&sha,digest)) ok=false;
    mbedtls_sha256_free(&sha);
    if(!ok) { ESP_LOGE(TAG,"RECEIVE incomplete; partial local stage retained; no source writes"); return false; }
    sha_hex(digest,record.sha256); record.data_crc32=crc^UINT32_MAX;
    char payload[PATH_SIZE],receipt[PATH_SIZE];
    if(!unused_receipt(&record,payload,receipt)) return false;
    ingest_record_seal(&record);
    if(rename(stage,payload) || !ingest_publish_record(receipt,&record,false)) {
        ESP_LOGE(TAG,"RECEIVE receipt publication failed; local payload retained"); return false;
    }
    if(!ingest_source_publish(STORE,&record)) ESP_LOGW(TAG,"Source index deferred; durable inbox receipt retained");
    received++;
    ESP_LOGI(TAG,"RECEIVE STORED source=%s receipt=%s bytes=%" PRIu64 " elapsed_ms=%lld; local readback/metadata deferred until source release",
             item->source,record.receipt_id,record.bytes,(long long)((esp_timer_get_time()-started)/1000));
    return true;
}
bool local_storage_receive(const char *source,bool (*available)(void))
{
    if(!card || !ensure_identity() || !valid_source(source) || !available || !available()) {
        ESP_LOGE(TAG,"RECEIVE needs local SD and available /usb source"); return false;
    }
    if(!ingest_layout(STORE,device_prefix)) return false;
    source_available=available; received=fast_skipped=staged_skipped=ignored=planned=recovery_planned=0;
    char path[PATH_SIZE]; snprintf(path,sizeof(path),STORE "/%s/inbox/.source.work",device_prefix);
    FILE *work=fopen(path,"wb"); if(!work) return false;
    int64_t started=esp_timer_get_time();
    bool ok=discover_sources(source,0,work);
    if(fflush(work)) ok=false;
    if(fclose(work)) ok=false;
    ESP_LOGI(TAG,"RECEIVE discovery mode=%s new_or_changed=%u old_recovery_or_audit=%u fast_skipped=%u staged_skipped=%u ignored=%u elapsed_ms=%lld",
             copy_verify_mode?"VERIFY":"FAST",planned-recovery_planned,recovery_planned,fast_skipped,staged_skipped,ignored,
             (long long)((esp_timer_get_time()-started)/1000));
    if(ok && planned) {
        work=fopen(path,"rb"); if(!work) ok=false;
        else {
            // Stream the small local worklist twice: all new content first,
            // known recovery/integrity-audit work last. No RAM file-count index.
            for(unsigned priority=0;ok && priority<2;priority++) {
                if(fseek(work,0,SEEK_SET)) { ok=false; break; }
                for(unsigned i=0;ok && i<planned;i++) {
                    receive_work_t item;
                    if(fread(&item,1,sizeof(item),work)!=sizeof(item) || item.crc32!=work_crc(&item) || item.priority>1 ||
                       !memchr(item.source,0,sizeof(item.source)) || !valid_source(item.source) || !media_file_supported(item.source) || item.bytes<0) { ok=false; break; }
                    if(item.priority==priority && !receive_one(&item)) ok=false;
                }
            }
            if(fclose(work)) ok=false;
        }
    }
    ESP_LOGI(TAG,"RECEIVE %s received=%u fast_skipped=%u staged_skipped=%u ignored=%u elapsed_ms=%lld; release USB before local processing",
             ok?"complete":"aborted",received,fast_skipped,staged_skipped,ignored,(long long)((esp_timer_get_time()-started)/1000));
    return ok;
}
static bool record_asset_location(const ingest_record_t *record,char *path)
{
    ingest_record_t asset;
    return (asset_record(record,true,&asset) || asset_record(record,false,&asset)) && asset_file(&asset,path);
}
static bool normalize_receipt(const char *receipt_path,const ingest_record_t *receipt,bool *created)
{
    ingest_record_t record=*receipt; char raw[PATH_SIZE],input[PATH_SIZE]; struct stat info;
    if(!ingest_payload_path(raw,sizeof(raw),STORE,&record)) return false;
    bool raw_exists=!stat(raw,&info);
    if(raw_exists) strcpy(input,raw);
    else if(errno!=ENOENT || !record_asset_location(&record,input)) {
        ESP_LOGE(TAG,"PROCESS missing durable payload receipt=%s",record.receipt_id); return false;
    }
    uint64_t bytes; uint32_t crc; char sha[65];
    if(!hash_file(input,false,&bytes,&crc,sha) || bytes!=record.bytes || crc!=record.data_crc32 || strcmp(sha,record.sha256)) {
        char bad[800]; snprintf(bad,sizeof(bad),"%s.bad",receipt_path);
        if(rename(receipt_path,bad)) ESP_LOGE(TAG,"Could not quarantine bad receipt; retained");
        ESP_LOGE(TAG,"PROCESS checksum failed; payload preserved, source index cannot skip this pending receipt"); return false;
    }
    capture_metadata_t metadata;
    if(!read_metadata_file(input,record.source_path,record.bytes,false,&metadata)) return false;
    bool linkable;
    if(!capture_identity(record.source_path,&metadata,record.sha256,record.capture_id,&linkable)) return false;
    strcpy(record.datetime,metadata.datetime); snprintf(record.time_source,sizeof(record.time_source),"%s",metadata.source);
    ingest_record_seal(&record);
    if(!reconcile_capture(record.capture_id,NULL,NULL)) return false;
    char final[PATH_SIZE],candidate[PATH_SIZE],directory[PATH_SIZE]; bool found=false;
    if(!archive_media_path(final,sizeof(final),STORE,device_prefix,record.source_path,&metadata,record.sha256,record.capture_id,"SD")) return false;
    const char *qualities[]={"SD","HD"};
    for(unsigned i=0;i<2;i++) {
        if(!archive_media_path(candidate,sizeof(candidate),STORE,device_prefix,record.source_path,&metadata,record.sha256,record.capture_id,qualities[i])) return false;
        if(!stat(candidate,&info)) { if(found) return false; found=true; strcpy(final,candidate); }
        else if(errno!=ENOENT) return false;
    }
    if(found) {
        if(strcmp(input,final) && (!hash_file(final,false,&bytes,&crc,sha) || bytes!=record.bytes || crc!=record.data_crc32 || strcmp(sha,record.sha256))) {
            ESP_LOGE(TAG,"Existing archive corrupt; inbox/source preserved for repair"); return false;
        }
    } else {
        if(!raw_exists) return false;
        char prepared[PATH_SIZE];
        if(!ingest_asset_path(prepared,sizeof(prepared),STORE,&record,false) || !ingest_publish_record(prepared,&record,true)) return false;
        strcpy(directory,final); *strrchr(directory,'/')=0;
        if(!mkdirs(directory) || rename(raw,final)) return false;
        raw_exists=false;
        if(!hash_file(final,false,&bytes,&crc,sha) || bytes!=record.bytes || crc!=record.data_crc32 || strcmp(sha,record.sha256)) return false;
        *created=true;
    }
    if(!reconcile_capture(record.capture_id,record.sha256,final)) return false;
    char ready[PATH_SIZE];
    if(!ingest_asset_path(ready,sizeof(ready),STORE,&record,true) || !ingest_publish_record(ready,&record,true) ||
       !ingest_source_publish(STORE,&record)) return false;
    // Only remove an independently verified duplicate local inbox payload.
    if(raw_exists && strcmp(raw,final) && unlink(raw)) return false;
    if(unlink(receipt_path)) return false;
    ESP_LOGI(TAG,"CAPTURE id=%s kind=%s known_time=%d source=%s storage=LOCAL",record.capture_id,record.kind,linkable,record.source_path);
    ESP_LOGI(TAG,"COPY %s source=%s destination=%s bytes=%" PRIu64 " CRC32=%08" PRIx32 " SHA256=%s phase=LOCAL_AFTER_RELEASE",
             *created?"VERIFIED":"SKIP verified",record.source_path,final,record.bytes,record.data_crc32,record.sha256);
    return true;
}
bool local_storage_process_inbox(void)
{
    if(!card || !ensure_identity() || !ingest_layout(STORE,device_prefix)) return false;
    char folder[PATH_SIZE],workpath[PATH_SIZE]; snprintf(folder,sizeof(folder),STORE "/%s/inbox",device_prefix);
    snprintf(workpath,sizeof(workpath),STORE "/%s/inbox/.process.work",device_prefix);
    FILE *work=fopen(workpath,"wb"); if(!work) return false;
    DIR *dir=opendir(folder); if(!dir) { fclose(work); return false; }
    bool ok=true; unsigned count=0,entries=0;
    for(;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if(!entry) { if(errno) ok=false; break; }
        if(++entries>131072) { ok=false; break; }
        if(strlen(entry->d_name)!=72 || strcmp(entry->d_name+64,".receipt")) continue;
        char id[65]; memcpy(id,entry->d_name,64); id[64]=0;
        if(!archive_hex_id(id) || count>=65535 || fwrite(id,1,sizeof(id),work)!=sizeof(id)) { ok=false; break; }
        count++;
    }
    if(closedir(dir)) ok=false;
    if(fflush(work)) ok=false;
    if(fclose(work)) ok=false;
    if(!ok) { ESP_LOGE(TAG,"PROCESS pending inventory incomplete; receipts preserved"); return false; }
    ESP_LOGI(TAG,"PROCESS begin pending=%u storage=LOCAL; USB not used",count);
    if(!count) return true;
    work=fopen(workpath,"rb"); if(!work) return false;
    unsigned normalized=0,existing=0,failed=0;
    for(unsigned i=0;i<count;i++) {
        char id[65],path[PATH_SIZE]; ingest_record_t record; bool created=false;
        if(fread(id,1,sizeof(id),work)!=sizeof(id) || id[64]!=0 || !archive_hex_id(id)) { failed+=count-i; break; }
        if(!ingest_receipt_path(path,sizeof(path),STORE,device_prefix,id) || !ingest_read_record(path,device_prefix,&record) ||
           strcmp(record.receipt_id,id) || !normalize_receipt(path,&record,&created)) failed++;
        else if(created) normalized++; else existing++;
    }
    if(fclose(work)) failed++;
    ESP_LOGI(TAG,"PROCESS complete normalized=%u verified_existing=%u failed=%u; source untouched",normalized,existing,failed);
    return !failed;
}

static bool group_reference(const char *reference,char id[65],char requested[160])
{
    requested[0]=0;
    if(archive_hex_id(reference)) { strcpy(id,reference); return true; }
    const char *name=strrchr(reference,'/'); name=name?name+1:reference;
    archive_asset_t asset; bool linkable;
    if(strlen(name)>=160 || !archive_parse_filename(name,device_prefix,&asset) || !asset.capture.datetime[0] ||
       !capture_identity(!strcmp(asset.kind,"PHOTO")?"media.jpg":"media.mp4",&asset.capture,asset.sha256,id,&linkable) || !linkable) {
        ESP_LOGE(TAG,"Use an archived filename (PHOTO/VIDEO, SD/HD) with a known camera time, or its capture ID"); return false;
    }
    strcpy(requested,name); return true;
}
static bool resolve_hd(const char *reference,transfer_job_t *job)
{
    memset(job,0,sizeof(*job)); strcpy(job->prefix,device_prefix);
    if(!group_reference(reference,job->capture_id,job->requested_filename) || !reconcile_capture(job->capture_id,NULL,NULL)) return false;
    if(job->requested_filename[0]) {
        archive_asset_t requested; char folder[PATH_SIZE],found[PATH_SIZE];
        if(!archive_parse_filename(job->requested_filename,device_prefix,&requested) || !group_folder(folder,job->capture_id) ||
           !media_group_find(folder,device_prefix,requested.sha256,found,sizeof(found))) {
            ESP_LOGE(TAG,"Requested filename's asset is not in this local capture group"); return false;
        }
    }
    if(!active_group.hd_path[0]) {
        ESP_LOGW(TAG,"HD UNAVAILABLE capture=%s files=%u largest_ties=%u; only SD or unknown time; nothing queued",
                 job->capture_id,active_group.rank.count,active_group.rank.max_count); return false;
    }
    strcpy(job->sha256,active_group.hd_sha256); strcpy(job->path,active_group.hd_path); job->bytes=active_group.rank.max_bytes;
    return true;
}
static bool verify_hd(const transfer_job_t *job)
{
    if(strcmp(job->prefix,device_prefix) || !reconcile_capture(job->capture_id,NULL,NULL) ||
       strcmp(job->sha256,active_group.hd_sha256) || strcmp(job->path,active_group.hd_path) || job->bytes!=active_group.rank.max_bytes) {
        ESP_LOGE(TAG,"Queued HD classification changed/missing; exact request will not be retargeted"); return false;
    }
    uint64_t bytes; uint32_t crc; char sha[65];
    return hash_file(job->path,false,&bytes,&crc,sha) && bytes==job->bytes && !strcmp(sha,job->sha256);
}
void local_storage_hd(const char *args)
{
    if(!card || !ensure_identity()) { ESP_LOGE(TAG,"HD queue requires mounted local SD; use SDMOUNT"); return; }
    if(args && !strncasecmp(args,"SHOW ",5)) {
        char id[65],name[160],folder[PATH_SIZE];
        if(group_reference(args+5,id,name) && group_folder(folder,id) && reconcile_capture(id,NULL,NULL)) {
            ESP_LOGI(TAG,"HD GROUP capture=%s files=%u HD=%d largest_ties=%u",id,active_group.rank.count,active_group.hd_path[0]!=0,active_group.rank.max_count);
            list_tree(folder,0);
        }
        return;
    }
    char folder[PATH_SIZE];
    int n=snprintf(folder,sizeof(folder),STORE "/%s/queue",device_prefix);
    if(n<0 || n>=sizeof(folder) || !mkdirs(folder)) { ESP_LOGE(TAG,"HD queue directory unavailable"); return; }
    transfer_queue_command(args,folder,device_prefix,resolve_hd,verify_hd);
}

void local_storage_journal(const char *event, uint32_t cycle, const char *result, const char *detail)
{
    if(!card) return;
    char directory[PATH_SIZE]=STORE;
    if(!mkdirs(directory)) { ESP_LOGE(TAG,"SOAK journal directory unavailable"); return; }
    FILE *file=fopen(STORE "/soak.csv","a");
    if(!file) { ESP_LOGE(TAG,"SOAK journal open failed errno=%d",errno); return; }
    bool ok=fprintf(file,"%lld,%s,%" PRIu32 ",%s,%s\n",
                    (long long)(esp_timer_get_time()/1000),event,cycle,result,detail)>=0;
    if(fflush(file)) ok=false;
    if(fsync(fileno(file))) ok=false;
    if(fclose(file)) ok=false;
    if(!ok) ESP_LOGE(TAG,"SOAK journal write failed; laptop UART log remains primary");
}
