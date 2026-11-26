#include "collection.h"
#include "media_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_FILES 128
#define FILE_PATH_SIZE 256
#define DEFAULT_INTERVAL_MS (6LL * 60 * 60 * 1000)
#define RETRY_MS 30000
#define MAX_ATTEMPTS 3

typedef struct {
    char path[FILE_PATH_SIZE];
    uint64_t size;
    int64_t mtime;
} media_file_t;

static const char *TAG="collector";
static media_file_t *baseline;
static size_t baseline_count;
static bool have_baseline;
static bool enabled;
static unsigned attempt;
static int64_t interval_ms=DEFAULT_INTERVAL_MS, next_ms;
static const char *last_result="none";

// Version hint only; actual archival equality still requires full CRC checks.
static uint64_t metadata_id(const media_file_t *file)
{
    uint64_t id=UINT64_C(14695981039346656037);
    for (const unsigned char *p=(const unsigned char *)file->path; *p; p++)
        id=(id^*p)*UINT64_C(1099511628211);
    for (unsigned i=0; i<8; i++) id=(id^((file->size>>(8*i))&255))*UINT64_C(1099511628211);
    for (unsigned i=0; i<8; i++) id=(id^(((uint64_t)file->mtime>>(8*i))&255))*UINT64_C(1099511628211);
    return id;
}

static bool walk(const char *path, unsigned depth, media_file_t *snapshot,
                 size_t *count, collection_ready_fn available)
{
    if (!available() || depth>16) {
        ESP_LOGE(TAG,"PLAN source unavailable or directory depth exceeded at %s",path); return false;
    }
    DIR *dir=opendir(path);
    if (!dir) { ESP_LOGE(TAG,"PLAN opendir %s errno=%d",path,errno); return false; }
    bool ok=true;
    for (;;) {
        errno=0;
        struct dirent *entry=readdir(dir);
        if (!entry) { if(errno) ok=false; break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[FILE_PATH_SIZE]; struct stat st;
        int n=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        if(n<0 || n>=sizeof(child) || !available() || stat(child,&st)) { ok=false; break; }
        if(S_ISDIR(st.st_mode)) {
            if(!walk(child,depth+1,snapshot,count,available)) { ok=false; break; }
        } else if(S_ISREG(st.st_mode) && media_file_supported(child)) {
            if(*count==MAX_FILES) {
                ESP_LOGE(TAG,"PLAN exceeds %u media files; previous baseline preserved",MAX_FILES); ok=false; break;
            }
            media_file_t *file=&snapshot[(*count)++];
            strcpy(file->path,child); file->size=st.st_size; file->mtime=st.st_mtime;
        }
        vTaskDelay(1);
    }
    if (closedir(dir)) ok=false;
    return ok && available();
}

static int path_compare(const void *a,const void *b)
{
    return strcmp(((const media_file_t *)a)->path,((const media_file_t *)b)->path);
}

bool collection_plan(collection_ready_fn available)
{
    if(!available || !available()) return false;
    media_file_t *snapshot=calloc(MAX_FILES,sizeof(*snapshot));
    if(!snapshot) { ESP_LOGE(TAG,"PLAN not enough RAM; previous baseline preserved"); return false; }
    size_t count=0;
    // Scope is the camera media trees, not system/metadata directories.
    const char *roots[]={"/usb/DCIM","/usb/cell_std","/usb/cell_hd"};
    bool ok=true;
    for(unsigned i=0;i<3;i++) {
        struct stat st;
        if(stat(roots[i],&st)) {
            if(errno==ENOENT && available()) continue;
            ok=false; break;
        }
        if(!S_ISDIR(st.st_mode) || !walk(roots[i],0,snapshot,&count,available)) { ok=false; break; }
    }
    if(!ok || !available()) {
        free(snapshot); ESP_LOGE(TAG,"PLAN aborted; previous baseline preserved"); return false;
    }
    qsort(snapshot,count,sizeof(*snapshot),path_compare);
    unsigned added=0,changed=0,unchanged=0,missing=0;
    uint64_t total=0,preview=0;
    for(size_t i=0;i<count;i++) {
        media_file_t *file=&snapshot[i]; total+=file->size;
        if(!strncmp(file->path,"/usb/cell_std/",14)) preview+=file->size;
        const media_file_t *old=baseline ? bsearch(file,baseline,baseline_count,sizeof(*baseline),path_compare) : NULL;
        const char *state;
        if(!old) { added++; state=have_baseline ? "NEW" : "BASELINE"; }
        else if(old->size!=file->size || old->mtime!=file->mtime) { changed++; state="CHANGED"; }
        else { unchanged++; state="UNCHANGED"; }
        ESP_LOGI(TAG,"PLAN %s id=%016" PRIx64 " path=%s bytes=%" PRIu64 " mtime=%" PRId64,
                 state,metadata_id(file),file->path,file->size,file->mtime);
    }
    for(size_t i=0;i<baseline_count;i++) {
        if(!bsearch(&baseline[i],snapshot,count,sizeof(*snapshot),path_compare)) {
            missing++; ESP_LOGI(TAG,"PLAN MISSING %s (no deletion performed)",baseline[i].path);
        }
    }
    ESP_LOGI(TAG,"PLAN complete files=%u bytes=%" PRIu64 " cell_std_bytes=%" PRIu64
             " new=%u changed=%u unchanged=%u missing=%u first_baseline=%d; DRY RUN ONLY",
             (unsigned)count,total,preview,have_baseline?added:0,changed,unchanged,missing,!have_baseline);
    free(baseline); baseline=snapshot; baseline_count=count; have_baseline=true;
    return true;
}

void collection_reset(void)
{
    free(baseline); baseline=NULL; baseline_count=0; have_baseline=false;
    ESP_LOGI(TAG,"PLAN RAM baseline cleared; source/local files untouched");
}

void collection_status(void)
{
    ESP_LOGI(TAG,"SCHEDULE enabled=%d mode=DRYRUN interval_seconds=%lld next_uptime_ms=%lld attempt=%u/%u last=%s baseline_files=%u baseline_valid=%d; main task controls power workflow",
             enabled,(long long)(interval_ms/1000),(long long)(enabled?next_ms:0),attempt,MAX_ATTEMPTS,
             last_result,(unsigned)baseline_count,have_baseline);
}

bool collection_command(const char *args,int64_t now_ms)
{
    if(!args || !*args || !strcasecmp(args,"STATUS")) collection_status();
    else if(!strcasecmp(args,"ON")) {
        enabled=true; attempt=0; next_ms=now_ms+interval_ms; collection_status();
    } else if(!strcasecmp(args,"OFF")) {
        enabled=false; attempt=0; collection_status();
    } else if(!strcasecmp(args,"NOW")) {
        // One immediate dry-run window; does not enable a disabled schedule.
        return true;
    } else if(!strncasecmp(args,"EVERY ",6)) {
        char *end; errno=0;
        unsigned long seconds=strtoul(args+6,&end,10);
        if(errno || end==args+6 || *end || seconds<30 || seconds>86400) {
            ESP_LOGW(TAG,"Use SCHEDULE EVERY seconds, range 30..86400");
        } else {
            interval_ms=(int64_t)seconds*1000; attempt=0;
            if(enabled) next_ms=now_ms+interval_ms;
            collection_status();
        }
    } else ESP_LOGW(TAG,"SCHEDULE ON | OFF | STATUS | NOW | EVERY seconds");
    return false;
}

bool collection_due(int64_t now_ms)
{
    return enabled && now_ms>=next_ms;
}

void collection_finished(bool success,int64_t now_ms)
{
    last_result=success?"plan_complete":"plan_failed";
    if(!enabled) return;
    if(success) { attempt=0; next_ms=now_ms+interval_ms; }
    else if(++attempt<MAX_ATTEMPTS) {
        next_ms=now_ms+RETRY_MS;
        ESP_LOGW(TAG,"Collection plan failed; next power/plan workflow in 30 seconds.");
    } else {
        attempt=0; next_ms=now_ms+interval_ms;
        ESP_LOGW(TAG,"Collection plan window exhausted; waiting until next interval");
    }
}
