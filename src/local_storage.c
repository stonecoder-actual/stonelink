#include "local_storage.h"
#include "media_filter.h"
#include "archive_naming.h"
#include "media_group.h"
#include "transfer_queue.h"
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
static unsigned copied, skipped, ignored;
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

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t n)
{
    for (size_t i=0; i<n; i++) {
        crc ^= data[i];
        for (unsigned bit=0; bit<8; bit++) crc=(crc>>1)^((crc&1) ? 0xedb88320u : 0);
    }
    return crc;
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
    mbedtls_sha256_context context; mbedtls_sha256_init(&context);
    bool ok=mbedtls_sha256_starts(&context,0)==0;
    while (ok && (n=fread(buffer, 1, sizeof(buffer), f))) {
        if (source && !source_available()) { ok=false; break; }
        count+=n; value=crc_update(value, buffer, n);
        if(mbedtls_sha256_update(&context,buffer,n)) { ok=false; break; }
        vTaskDelay(1);
    }
    if (ferror(f) || (source && !source_available())) ok=false;
    if (fclose(f)) ok=false;
    uint8_t digest[32];
    if(ok && mbedtls_sha256_finish(&context,digest)) ok=false;
    mbedtls_sha256_free(&context);
    if (!ok) { ESP_LOGE(TAG, "Incomplete checksum read %s", path); return false; }
    *bytes=count; *crc=value^0xffffffffu;
    sha_hex(digest,sha);
    return true;
}

typedef struct { FILE *file; bool io_error; } metadata_file_t;
static bool metadata_read_at(void *context,uint64_t offset,void *out,size_t length)
{
    metadata_file_t *reader=context;
    if(reader->io_error) return false; // One failed USB read ends further metadata I/O.
    if(!source_available() || offset>LONG_MAX || fseek(reader->file,(long)offset,SEEK_SET) ||
       fread(out,1,length,reader->file)!=length) { reader->io_error=true; return false; }
    return true;
}
static bool read_metadata(const char *source,uint64_t size,capture_metadata_t *metadata)
{
    FILE *file=fopen(source,"rb");
    if(!file) { ESP_LOGE(TAG,"META open failed %s errno=%d",source,errno); return false; }
    metadata_file_t reader={file,false};
    const char *ext=strrchr(source,'.');
    capture_metadata_read(metadata_read_at,&reader,size,strcasecmp(ext,".mp4")!=0,metadata);
    bool ok=!reader.io_error && !ferror(file) && source_available();
    if(fclose(file)) ok=false;
    if(ok) ESP_LOGI(TAG,"META source=%s camera_datetime=%s time_source=%s timezone=camera_unspecified conversion=NONE",
                   source,metadata->datetime[0]?metadata->datetime:"UNKNOWN_CAPTURE_TIME",metadata->source);
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
    read_metadata(source,st.st_size,&metadata);
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
static bool copy_file(const char *source, const struct stat *before)
{
    uint64_t size; uint32_t crc; char sha[65];
    ESP_LOGI(TAG, "COPY checking %s size=%lld", source, (long long)before->st_size);
    if (!hash_file(source, true, &size, &crc, sha) || size != (uint64_t)before->st_size) return false;
    capture_metadata_t metadata;
    if(!ensure_identity() || !read_metadata(source,size,&metadata)) return false;
    char capture_id[65]; bool linkable;
    if(!capture_identity(source,&metadata,sha,capture_id,&linkable)) return false;
    char directory[PATH_SIZE], final[PATH_SIZE],candidate[PATH_SIZE];
    if(!reconcile_capture(capture_id,NULL,NULL) ||
       !archive_media_path(final,sizeof(final),STORE,device_prefix,source,&metadata,sha,capture_id,"SD")) return false;
    ESP_LOGI(TAG,"CAPTURE id=%s kind=%s known_time=%d source=%s",capture_id,archive_kind(source),linkable,source);
    struct stat existing; bool found=false;
    const char *qualities[]={"SD","HD"};
    for(unsigned i=0;i<2;i++) {
        if(!archive_media_path(candidate,sizeof(candidate),STORE,device_prefix,source,&metadata,sha,capture_id,qualities[i])) return false;
        if(!stat(candidate,&existing)) {
            if(found) { ESP_LOGE(TAG,"Duplicate archive aliases; preserved"); return false; }
            found=true; strcpy(final,candidate);
        } else if(errno!=ENOENT) { ESP_LOGE(TAG,"Archive stat failed errno=%d",errno); return false; }
    }
    if (found) {
        if(stat(final,&existing)) return false;
        uint64_t local_size; uint32_t local_crc; char local_sha[65];
        if (!S_ISREG(existing.st_mode) || !hash_file(final, false, &local_size, &local_crc,local_sha) || local_size!=size || local_crc!=crc || strcmp(sha,local_sha)) {
            ESP_LOGE(TAG, "Existing archive differs/cannot verify; preserved: %s", final); return false;
        }
        struct stat after;
        if(!source_available() || stat(source,&after) || after.st_size!=before->st_size || after.st_mtime!=before->st_mtime ||
           !reconcile_capture(capture_id,sha,final)) return false;
        skipped++;
        ESP_LOGI(TAG, "COPY SKIP verified %s bytes=%" PRIu64 " CRC32=%08" PRIx32 " SHA256=%s", final, size, crc,sha);
        return true;
    }
    // Reopen source for copying. A second CRC must match the pre-copy checksum.
    strcpy(directory, STORE);
    if (!mkdirs(directory)) return false;
    FILE *in=fopen(source, "rb"), *out=NULL;
    if (in) out=fopen(STAGING, "wb"); // Reserved staging file: overwritten after interrupted copies.
    if (!in || !out) {
        if (in) fclose(in);
        ESP_LOGE(TAG, "COPY open failed errno=%d", errno); return false;
    }
    mbedtls_sha256_context streaming_sha; mbedtls_sha256_init(&streaming_sha);
    uint64_t written=0; uint32_t streaming=0xffffffffu; size_t n;
    bool ok=mbedtls_sha256_starts(&streaming_sha,0)==0;
    while (ok && (n=fread(buffer, 1, sizeof(buffer), in))) {
        if (!source_available() || fwrite(buffer, 1, n, out)!=n) { ok=false; break; }
        written+=n; streaming=crc_update(streaming, buffer, n);
        if(mbedtls_sha256_update(&streaming_sha,buffer,n)) ok=false;
        vTaskDelay(1);
    }
    if (ferror(in) || !source_available() || written!=size || (streaming^0xffffffffu)!=crc) ok=false;
    if (fclose(in)) ok=false;
    if (fflush(out)) ok=false;
    if (fsync(fileno(out))) ok=false;
    if (fclose(out)) ok=false;
    uint8_t streamed_digest[32]; char streamed_text[65];
    if(ok) {
        if(mbedtls_sha256_finish(&streaming_sha,streamed_digest)) ok=false;
        else { sha_hex(streamed_digest,streamed_text); if(strcmp(sha,streamed_text)) ok=false; }
    }
    mbedtls_sha256_free(&streaming_sha);
    struct stat after;
    if (stat(source, &after) || after.st_size!=before->st_size || after.st_mtime!=before->st_mtime) ok=false;
    uint64_t verified_size=0; uint32_t verified_crc=0; char verified_sha[65];
    if (ok) ok=hash_file(STAGING, false, &verified_size, &verified_crc,verified_sha) && verified_size==size && verified_crc==crc && !strcmp(sha,verified_sha);
    if (!ok) {
        ESP_LOGE(TAG, "COPY failed/interrupted; source untouched; staging retained for next attempt"); return false;
    }
    strcpy(directory, final);
    *strrchr(directory, '/')=0;
    if (!mkdirs(directory) || rename(STAGING, final)) {
        ESP_LOGE(TAG, "COPY publish failed errno=%d; staging retained", errno); return false;
    }
    if (!hash_file(final, false, &verified_size, &verified_crc,verified_sha) || verified_size!=size || verified_crc!=crc || strcmp(sha,verified_sha)) {
        ESP_LOGE(TAG, "COPY final verification failed; source untouched: %s", final); return false;
    }
    if(!reconcile_capture(capture_id,sha,final)) return false;
    copied++;
    ESP_LOGI(TAG, "COPY VERIFIED source=%s destination=%s bytes=%" PRIu64 " CRC32=%08" PRIx32 " SHA256=%s", source, final, size, crc,sha);
    return true;
}

static bool copy_tree(const char *path, unsigned depth)
{
    if (!source_available() || depth>16) return false;
    struct stat st;
    if (stat(path, &st)) { ESP_LOGE(TAG, "Source stat failed %s errno=%d", path, errno); return false; }
    if (S_ISREG(st.st_mode)) {
        if(!media_file_supported(path)) {
            ignored++;
            ESP_LOGI(TAG,"COPY IGNORE non-media %s",path);
            return true;
        }
        return copy_file(path, &st);
    }
    if (!S_ISDIR(st.st_mode)) return false;
    DIR *dir=opendir(path);
    if (!dir) return false;
    struct dirent *entry; bool ok=true;
    for (;;) {
        errno=0; entry=readdir(dir);
        if (!entry) { if (errno) { ESP_LOGE(TAG, "readdir %s errno=%d", path, errno); ok=false; } break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_SIZE];
        if (!joined(child, path, entry->d_name) || !copy_tree(child, depth+1)) { ok=false; break; }
    }
    closedir(dir);
    return ok;
}

bool local_storage_copy(const char *source, bool (*available)(void))
{
    if (!card) { ESP_LOGE(TAG, "COPY requires mounted local SD; use SDMOUNT/SDSTATUS"); return false; }
    if (!valid_source(source) || !available || !available()) { ESP_LOGE(TAG, "COPY requires available /usb source path"); return false; }
    source_available=available; copied=skipped=ignored=0;
    bool ok=copy_tree(source, 0);
    ESP_LOGI(TAG, "COPY %s copied=%u skipped=%u ignored=%u; source files untouched", ok ? "complete" : "aborted", copied, skipped, ignored);
    return ok;
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
