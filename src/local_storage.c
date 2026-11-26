#include "local_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ff.h"
#include "esp_timer.h"

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
static unsigned copied, skipped;

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

static bool hash_file(const char *path, bool source, uint64_t *bytes, uint32_t *crc)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Read open failed %s errno=%d", path, errno); return false; }
    uint64_t count=0; uint32_t value=0xffffffffu; size_t n;
    bool ok=true;
    while ((n=fread(buffer, 1, sizeof(buffer), f))) {
        if (source && !source_available()) { ok=false; break; }
        count+=n; value=crc_update(value, buffer, n);
        vTaskDelay(1);
    }
    if (ferror(f) || (source && !source_available())) ok=false;
    if (fclose(f)) ok=false;
    if (!ok) { ESP_LOGE(TAG, "Incomplete checksum read %s", path); return false; }
    *bytes=count; *crc=value^0xffffffffu;
    return true;
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

static bool copy_file(const char *source, const struct stat *before)
{
    uint64_t size; uint32_t crc;
    ESP_LOGI(TAG, "COPY checking %s size=%lld", source, (long long)before->st_size);
    if (!hash_file(source, true, &size, &crc) || size != (uint64_t)before->st_size) return false;
    char directory[PATH_SIZE], final[PATH_SIZE];
    int len=snprintf(final, sizeof(final), STORE "/%08" PRIx32 "_%" PRIu64 "/%s", crc, size, source+5);
    if (len<0 || len>=sizeof(final)) return false;
    struct stat existing;
    if (stat(final, &existing)==0) {
        uint64_t local_size; uint32_t local_crc;
        if (!S_ISREG(existing.st_mode) || !hash_file(final, false, &local_size, &local_crc) || local_size!=size || local_crc!=crc) {
            ESP_LOGE(TAG, "Existing archive differs/cannot verify; preserved: %s", final); return false;
        }
        skipped++;
        ESP_LOGI(TAG, "COPY SKIP verified %s bytes=%" PRIu64 " CRC32=%08" PRIx32, final, size, crc);
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
    uint64_t written=0; uint32_t streaming=0xffffffffu; size_t n; bool ok=true;
    while ((n=fread(buffer, 1, sizeof(buffer), in))) {
        if (!source_available() || fwrite(buffer, 1, n, out)!=n) { ok=false; break; }
        written+=n; streaming=crc_update(streaming, buffer, n); vTaskDelay(1);
    }
    if (ferror(in) || !source_available() || written!=size || (streaming^0xffffffffu)!=crc) ok=false;
    if (fclose(in)) ok=false;
    if (fflush(out)) ok=false;
    if (fsync(fileno(out))) ok=false;
    if (fclose(out)) ok=false;
    struct stat after;
    if (stat(source, &after) || after.st_size!=before->st_size || after.st_mtime!=before->st_mtime) ok=false;
    uint64_t verified_size=0; uint32_t verified_crc=0;
    if (ok) ok=hash_file(STAGING, false, &verified_size, &verified_crc) && verified_size==size && verified_crc==crc;
    if (!ok) {
        ESP_LOGE(TAG, "COPY failed/interrupted; source untouched; staging retained for next attempt"); return false;
    }
    strcpy(directory, final);
    *strrchr(directory, '/')=0;
    if (!mkdirs(directory) || rename(STAGING, final)) {
        ESP_LOGE(TAG, "COPY publish failed errno=%d; staging retained", errno); return false;
    }
    if (!hash_file(final, false, &verified_size, &verified_crc) || verified_size!=size || verified_crc!=crc) {
        ESP_LOGE(TAG, "COPY final verification failed; source untouched: %s", final); return false;
    }
    copied++;
    ESP_LOGI(TAG, "COPY VERIFIED source=%s destination=%s bytes=%" PRIu64 " CRC32=%08" PRIx32, source, final, size, crc);
    return true;
}

static bool copy_tree(const char *path, unsigned depth)
{
    if (!source_available() || depth>16) return false;
    struct stat st;
    if (stat(path, &st)) { ESP_LOGE(TAG, "Source stat failed %s errno=%d", path, errno); return false; }
    if (S_ISREG(st.st_mode)) return copy_file(path, &st);
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
    source_available=available; copied=skipped=0;
    bool ok=copy_tree(source, 0);
    ESP_LOGI(TAG, "COPY %s copied=%u skipped=%u; source files untouched", ok ? "complete" : "aborted", copied, skipped);
    return ok;
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
