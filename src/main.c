#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "esp_private/msc_scsi_bot.h"
#include "ff.h"
#include "diskio.h"
#include "local_storage.h"
#include "collection.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "power_cycle.h"
#include "media_io.h"

// Fail closed: only the mounted local SD's registered drive may write.
// USB and unknown drives remain protected, regardless of mount order.
DRESULT __real_ff_disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT __wrap_ff_disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (local_storage_write_allowed(pdrv)) return __real_ff_disk_write(pdrv, buff, sector, count);
    ESP_LOGE("write_guard", "Blocked source/unknown drive write pdrv=%u", pdrv);
    return RES_WRPRT;
}

static const char *TAG = "stonelink";
#define LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
static QueueHandle_t events;
static msc_host_device_handle_t disk;
static msc_host_vfs_handle_t vfs;
static usb_host_client_handle_t observer;
static usb_device_handle_t observed[128];
static bool watch;
static uint8_t address;
static atomic_bool removed;
static char first_jpeg[512];
static size_t sector_size;
static uint8_t probe[4096] __attribute__((aligned(4)));
static unsigned usb_attempts;
static bool usb_retry_paused;
static int64_t usb_retry_after;
#define USB_MAX_ATTEMPTS 3
static unsigned soak_mode; // 0=off, 1=metadata monitor, 2=verified copies
static uint32_t soak_seconds=300;
static int64_t soak_next, soak_heartbeat;
static uint32_t soak_cycles,soak_success,soak_failures;
static uint32_t soak_connections,soak_removals;
static bool soak_persistent;
typedef enum { JOB_NONE, JOB_PLAN, JOB_COPY, JOB_SCHEDULE, JOB_SOAK_PLAN, JOB_SOAK_COPY } job_kind_t;
static job_kind_t job;
static uint32_t job_id;
static int64_t job_started;
static bool power_managed, power_gpio_ready;
static char job_source[768]="/usb";
static void start_collection(job_kind_t kind);
static bool check(esp_err_t e, const char *what) {
    if (e != ESP_OK) ESP_LOGE(TAG, "%s: %s (0x%x)", what, esp_err_to_name(e), e);
    return e == ESP_OK;
}
static void daemon_task(void *arg) {
    for (;;) {
        uint32_t flags = 0;
        esp_err_t e = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
        if (e != ESP_ERR_TIMEOUT) check(e, "USB library events");
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) check(usb_host_device_free_all(), "free devices");
    }
}
static void msc_event(const msc_host_event_t *event, void *arg) {
    if (event->event == MSC_DEVICE_DISCONNECTED) removed = true;
    if (xQueueSend(events, event, 0) != pdTRUE) ESP_LOGE(TAG, "MSC event queue overflow");
}
static void observer_event(const usb_host_client_event_msg_t *event, void *arg) {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        usb_device_handle_t dev;
        uint8_t addr = event->new_dev.address;
        LOG("USB device connected address=%u", addr);
        soak_connections++;
        if (!check(usb_host_device_open(observer, addr, &dev), "descriptor open")) return;
        observed[addr] = dev;
        const usb_device_desc_t *desc;
        if (check(usb_host_get_device_descriptor(dev, &desc), "device descriptor")) {
            LOG("VID=%04x PID=%04x USB=%04x class=%02x configurations=%u", desc->idVendor, desc->idProduct, desc->bcdUSB, desc->bDeviceClass, desc->bNumConfigurations);
            usb_print_device_descriptor(desc);
        }
        const usb_config_desc_t *cfg;
        if (check(usb_host_get_active_config_descriptor(dev, &cfg), "configuration descriptor")) {
            usb_print_config_descriptor(cfg, NULL);
            const uint8_t *p = (const uint8_t *)cfg;
            size_t offset = 0;
            while (offset + 2 <= cfg->wTotalLength) {
                uint8_t len = p[offset], type = p[offset+1];
                if (len < 2 || offset + len > cfg->wTotalLength) break;
                if (type == 4 && len >= 9) {
                    LOG("Interface %u alt=%u class=%02x subclass=%02x protocol=%02x", p[offset+2], p[offset+3], p[offset+5], p[offset+6], p[offset+7]);
                    if (p[offset+5] == 8) LOG("Mass storage interface detected");
                }
                offset += len;
            }
        }
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        LOG("USB device removed");
        soak_removals++;
        for (unsigned i=1; i<128; i++) if (observed[i] == event->dev_gone.dev_hdl) {
            if (address == i) { removed=true; address=0; }
            if (check(usb_host_device_close(observer, observed[i]), "observer close")) observed[i]=NULL;
        }
    }
}
static bool release_disk(void) {
    if (vfs) {
        if (!check(msc_host_vfs_unregister(vfs), "unmount")) return false;
        vfs = NULL;
    }
    if (disk) {
        if (!check(msc_host_uninstall_device(disk), "release MSC")) return false;
        disk = NULL;
    }
    first_jpeg[0] = 0;
    return true;
}
static void scan(const char *path, unsigned depth) {
    if (removed) return;
    if (depth > 16) { ESP_LOGW(TAG, "Directory depth limit at %s", path); return; }
    DIR *dir = opendir(path);
    if (!dir) { ESP_LOGE(TAG, "opendir %s errno=%d", path, errno); return; }
    struct dirent *entry;
    while (!removed && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[512];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= sizeof(child)) { ESP_LOGW(TAG,"Path too long"); continue; }
        struct stat st;
        if (stat(child, &st)) { ESP_LOGE(TAG,"stat %s errno=%d",child,errno); continue; }
        LOG("%s size=%lld mtime=%lld (FAT local time, timezone unknown)", child, (long long)st.st_size, (long long)st.st_mtime);
        const char *ext = strrchr(child,'.');
        if (S_ISREG(st.st_mode) && ext && (!strcasecmp(ext,".jpg") || !strcasecmp(ext,".jpeg")) && !first_jpeg[0]) strcpy(first_jpeg,child);
        if (S_ISDIR(st.st_mode)) scan(child, depth+1);
    }
    closedir(dir);
}
static void read_file(const char *path) {
    if (!vfs || removed) { LOG("No mounted storage"); return; }
    if (strncmp(path,"/usb/",5) || strstr(path,"..")) { LOG("Use READ /usb/path/to/file.jpg"); return; }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) { LOG("READ requires a regular file: %s", path); return; }
    FILE *f = fopen(path,"rb");
    if (!f) { ESP_LOGE(TAG,"open %s errno=%d",path,errno); return; }
    uint8_t buf[4096]; uint32_t crc=0xffffffff; uint64_t total=0; size_t n;
    media_io_guard_t guard; media_io_begin(&guard);
    while (!removed && (n=fread(buf,1,sizeof(buf),f))) {
        total+=n;
        crc=media_crc32_update(crc,buf,n);
        media_io_checkpoint(&guard);
    }
    bool ok = !removed && !ferror(f) && total == (uint64_t)st.st_size;
    fclose(f);
    if (ok) LOG("READ %s bytes=%" PRIu64 " CRC32/ISO-HDLC=%08" PRIx32, path,total,crc^0xffffffff);
    else ESP_LOGE(TAG,"READ incomplete %s bytes=%" PRIu64,path,total);
}
static bool attach(uint8_t addr, bool diagnostics) {
    if (disk) return vfs != NULL;
    removed=false; address=addr;
    if (!check(msc_host_install_device(addr,&disk),"MSC readiness/install")) return false;
    msc_host_device_info_t info = {0};
    if (check(msc_host_get_device_info(disk,&info),"capacity")) LOG("Storage ready VID=%04x PID=%04x sectors=%" PRIu32 " sector_size=%" PRIu32 " capacity=%" PRIu64,info.idVendor,info.idProduct,info.sector_count,info.sector_size,(uint64_t)info.sector_count*info.sector_size);
    sector_size=info.sector_size;
    esp_vfs_fat_mount_config_t mount = {.format_if_mount_failed=false,.max_files=4,.allocation_unit_size=0};
    if (!check(msc_host_vfs_register(disk,"/usb",&mount,&vfs),"read-only FAT mount")) return false;
    LOG("FAT filesystem mounted; disk_write barrier enabled; FF_FS_READONLY=%d",FF_FS_READONLY);
    if (diagnostics) {
        scan("/usb",0); LOG("Storage scan %s", removed ? "interrupted" : "complete");
        if (first_jpeg[0]) read_file(first_jpeg);
        else LOG("No JPEG discovered");
    }
    return !removed;
}
static void acquisition_result(bool success) {
    if (success) { usb_attempts=0; usb_retry_after=0; usb_retry_paused=false; return; }
    usb_attempts++;
    usb_retry_after=esp_timer_get_time()/1000 + (usb_attempts==1 ? 15000 : 30000);
    if (usb_attempts>=USB_MAX_ATTEMPTS) {
        usb_retry_paused=true;
        ESP_LOGW(TAG,"USB acquisition paused after 3 failures; manual VBUS recovery may be needed. RETRY starts a fresh attempt window.");
    }
}
static void status(void) { LOG("STATUS address=%u installed=%d mounted=%d removed=%d WATCH=%d usb_failures=%u/%u retry_paused=%d uptime_ms=%lld",address,disk!=NULL,vfs!=NULL,removed,watch,usb_attempts,USB_MAX_ATTEMPTS,usb_retry_paused,(long long)(esp_timer_get_time()/1000)); }
static bool power_drive(bool on) {
    // Set the latch before enabling output so selecting GPIO mode starts LOW.
    if(!power_gpio_ready) {
        if(!check(gpio_set_level(GPIO_NUM_21,0),"GPIO21 initial LOW")) return false;
        gpio_config_t cfg={.pin_bit_mask=1ULL<<21,.mode=GPIO_MODE_OUTPUT,
                           .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,
                           .intr_type=GPIO_INTR_DISABLE};
        if(!check(gpio_config(&cfg),"GPIO21 output")) return false;
        power_gpio_ready=true;
    }
    return check(gpio_set_level(GPIO_NUM_21,on?1:0),"GPIO21 power command");
}
static void power_notice(const char *message) {
    LOG("%s",message);
    local_storage_journal("power",job_id,"INFO",message);
}
static void power_status(void) {
    LOG("POWER STATUS mode=%s pin=21 active_high=1 phase=%s job=%" PRIu32
        " busy=%d attempts=%u/3 deadline_uptime_ms=%lld; external supply only; VBUS not measured; mode not persisted",
        power_cycle_mode()==POWER_GPIO?"GPIO":"MANUAL",power_cycle_phase_name(),job_id,
        power_cycle_busy(),power_cycle_attempts(),(long long)power_cycle_deadline());
}
static bool source_available(void) { return vfs && disk && !removed; }
static bool run_plan(void) {
    bool ok=false;
    // Fresh mount avoids intentionally reusing a previous FatFS directory cache.
    // No USB reset or VBUS switch is issued by a collection attempt.
    if (!address || removed) LOG("PLAN needs USB device; enable source power after camera standby");
    else if (release_disk() && attach(address,false)) ok=collection_plan(source_available);
    else LOG("PLAN could not acquire USB storage; manual USB power cycle may be needed");
    if (!release_disk()) ok=false;
    // Keep a completed/failed collection window from being followed by unrelated
    // background mount retries. New USB attachment or RETRY can acquire again.
    usb_retry_paused=true;
    LOG("PLAN source released; no files copied/deleted; USB 5 V unchanged");
    return ok;
}
static void soak_save(void) {
    if(!soak_persistent) { ESP_LOGW(TAG,"SOAK config is RAM-only; NVS unavailable"); return; }
    nvs_handle_t handle;
    esp_err_t err=nvs_open("stonelink",NVS_READWRITE,&handle);
    if(err==ESP_OK) {
        err=nvs_set_u32(handle,"soak_mode",soak_mode);
        if(err==ESP_OK) err=nvs_set_u32(handle,"soak_sec",soak_seconds);
        if(err==ESP_OK) err=nvs_commit(handle);
        nvs_close(handle);
    }
    if(!check(err,"persist SOAK config")) soak_persistent=false;
}
static void soak_load(void) {
    esp_err_t err=nvs_flash_init(); // Never erase NVS to repair an init failure.
    soak_persistent=check(err,"NVS init");
    if(!soak_persistent) return;
    nvs_handle_t handle;
    if(nvs_open("stonelink",NVS_READONLY,&handle)!=ESP_OK) return;
    uint32_t mode=0,seconds=300;
    nvs_get_u32(handle,"soak_mode",&mode); nvs_get_u32(handle,"soak_sec",&seconds);
    nvs_close(handle);
    if(mode<=2 && seconds>=60 && seconds<=86400) {
        soak_mode=mode; soak_seconds=seconds;
    }
}
static void soak_status(void) {
    LOG("SOAK HEALTH mode=%u interval_s=%" PRIu32 " cycles=%" PRIu32 " success=%" PRIu32
        " failed=%" PRIu32 " connections=%" PRIu32 " removals=%" PRIu32
        " heap_free=%u heap_min=%u heap_largest=%u main_stack_watermark=%u next_uptime_ms=%lld persistent=%d",
        soak_mode,soak_seconds,soak_cycles,soak_success,soak_failures,soak_connections,soak_removals,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),(long long)soak_next,soak_persistent);
    status();
    power_status();
    char detail[160];
    snprintf(detail,sizeof(detail),"mode=%u;heap=%u;heap_min=%u;usb=%u;mounted=%u",
             soak_mode,(unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size(),address,vfs!=NULL);
    local_storage_journal("health",soak_cycles,"INFO",detail);
}
static bool job_is_soak(void) { return job==JOB_SOAK_PLAN || job==JOB_SOAK_COPY; }
static bool job_is_copy(void) { return job==JOB_COPY || job==JOB_SOAK_COPY; }
static void collection_complete(bool ok) {
    int64_t now=esp_timer_get_time()/1000;
    LOG("COLLECT end=%" PRIu32 " action=%s result=%s duration_ms=%lld; source untouched; external supply state not measured",
        job_id,job_is_copy()?"COPY":"PLAN",ok?"SUCCESS":"FAILED",(long long)(now-job_started));
    local_storage_journal("collect_end",job_id,ok?"SUCCESS":"FAILED","source_untouched");
    if(job_is_soak()) {
        if(ok) soak_success++; else soak_failures++;
        LOG("SOAK CYCLE end=%" PRIu32 " result=%s; source untouched; power workflow completed or aborted",
            soak_cycles,ok?"SUCCESS":"FAILED");
        local_storage_journal("cycle_end",soak_cycles,ok?"SUCCESS":"FAILED","power_workflow;source_untouched");
        soak_next=now+(int64_t)soak_seconds*1000;
        soak_status();
    } else if(job==JOB_SCHEDULE) collection_finished(ok,now);
    usb_retry_paused=true;
    job=JOB_NONE;
}
static void start_collection(job_kind_t kind) {
    if(job!=JOB_NONE || power_cycle_busy()) { LOG("COLLECT busy; use POWER STATUS"); return; }
    job=kind; job_id++; job_started=esp_timer_get_time()/1000;
    strcpy(job_source,"/usb");
    watch=false; power_managed=true; usb_retry_paused=true;
    if(job_is_soak()) {
        soak_cycles++;
        LOG("SOAK CYCLE start=%" PRIu32 " mode=%u",soak_cycles,soak_mode);
        local_storage_journal("cycle_start",soak_cycles,"INFO",job_is_copy()?"copy":"monitor");
    }
    LOG("COLLECT start=%" PRIu32 " action=%s power=%s; wait for camera recording to finish before starting",
        job_id,job_is_copy()?"COPY":"PLAN",power_cycle_mode()==POWER_GPIO?"GPIO":"MANUAL");
    // Never interrupt supply while the filesystem or MSC driver is owned.
    if(!release_disk() || (job_is_copy() && !local_storage_mount())) {
        LOG("COLLECT preflight failed; external supply unchanged"); collection_complete(false); return;
    }
    power_cycle_begin(esp_timer_get_time()/1000);
}
static void service_collection(void) {
    if(job==JOB_NONE) return;
    if(power_cycle_tick(esp_timer_get_time()/1000,address && !removed)) {
        LOG("COLLECT acquisition job=%" PRIu32 " attempt=%u/3 address=%u",job_id,power_cycle_attempts(),address);
        if(!release_disk()) {
            // An unreleased source is not safe to power off automatically.
            LOG("COLLECT release failed; canceling power workflow without further supply changes");
            power_cycle_abandon(); return;
        }
        if(!attach(address,false)) {
            if(release_disk()) power_cycle_acquisition_failed(esp_timer_get_time()/1000);
            else {
                LOG("COLLECT cleanup failed; supply unchanged; REINIT required");
                power_cycle_abandon(); return;
            }
        } else {
            bool ok=job_is_copy()?local_storage_receive(job_source,source_available):collection_plan(source_available);
            if(!release_disk()) {
                LOG("COLLECT cleanup failed; supply unchanged; REINIT required");
                power_cycle_abandon(); return;
            }
            LOG("COLLECT source-work result=%s; source released; local processing waits for final OFF",ok?"SUCCESS":"FAILED");
            power_cycle_finish(ok,esp_timer_get_time()/1000);
        }
    }
    bool ok;
    if(power_cycle_result(&ok)) {
        if(job_is_copy()) {
            if(!disk && !vfs && power_cycle_off_confirmed()) {
                bool processed=local_storage_process_inbox(); ok=ok && processed;
            } else LOG("Local inbox processing deferred; finish external OFF, then use PROCESS");
        }
        collection_complete(ok);
    }
}
static void soak_run(void) { start_collection(soak_mode==2?JOB_SOAK_COPY:JOB_SOAK_PLAN); }
static void soak_command(const char *arg) {
    int64_t now=esp_timer_get_time()/1000;
    if(!arg || !*arg || !strcasecmp(arg,"STATUS")) soak_status();
    else if(!strcasecmp(arg,"OFF")) {
        if(job_is_soak()) power_cycle_cancel();
        soak_mode=0; soak_save(); soak_status();
    }
    else if(!strcasecmp(arg,"ON") || !strcasecmp(arg,"COPY")) {
        bool copy=!strcasecmp(arg,"COPY");
        if(copy && !local_storage_mount()) { LOG("SOAK COPY not enabled: local card mount failed"); return; }
        if(!release_disk()) return;
        soak_mode=copy?2:1; soak_seconds=copy?21600:300;
        watch=false; collection_command("OFF",now); usb_retry_paused=true;
        soak_next=now+30000; soak_heartbeat=now; soak_save();
        power_managed=true;
        LOG("SOAK enabled: first cycle in 30 seconds; mode %u (1=monitor,2=copy); power workflow requires manual confirmations unless POWER MODE GPIO selected",soak_mode);
        soak_status();
    } else if(!strncasecmp(arg,"EVERY ",6)) {
        char *end; errno=0; unsigned long seconds=strtoul(arg+6,&end,10);
        if(errno || end==arg+6 || *end || seconds<60 || seconds>86400) { LOG("SOAK EVERY accepts 60..86400 seconds"); return; }
        soak_seconds=seconds; soak_next=now+(int64_t)seconds*1000; soak_save(); soak_status();
    } else if(!strcasecmp(arg,"NOW")) {
        if(soak_mode) soak_run(); else LOG("Enable SOAK ON or SOAK COPY first");
    } else LOG("SOAK ON | COPY | OFF | STATUS | NOW | EVERY seconds");
}
static void command(char *line) {
    char *arg=strchr(line,' '); if(arg) { *arg++=0; while(*arg==' ')arg++; }
    bool soak_control=!strcasecmp(line,"SOAK") && (!arg || !*arg || !strcasecmp(arg,"STATUS") || !strcasecmp(arg,"OFF"));
    if(power_cycle_busy() && strcasecmp(line,"POWER") && strcasecmp(line,"STATUS") && strcasecmp(line,"HELP") && !soak_control) {
        LOG("Power workflow active; use POWER STATUS, POWER OFF/ON, POWER CANCEL or SOAK OFF"); return;
    }
    if (!strcasecmp(line,"HELP")) LOG("HELP | STATUS | DEVICE | COPYMODE [FAST|VERIFY|STATUS] | PROCESS | META /usb/path | HD [SHOW filename_or_capture_id|LIST|NEXT|REQUEST filename_or_capture_id|CANCEL request_id|RETRY request_id] | POWER [STATUS|MODE MANUAL|MODE GPIO|OFF|ON|CANCEL] | COLLECT [PLAN|COPY] | SCAN | READ /usb/path | RESET | REINIT | RETRY | WATCH [ON|OFF] | PLAN [RESET] | SCHEDULE [ON|OFF|NOW|STATUS|EVERY seconds] | SDMOUNT | SDSTATUS | SDSCAN | COPY /usb/file | COPYALL | SOAK [ON|COPY|OFF|STATUS|NOW|EVERY seconds]");
    else if(!strcasecmp(line,"COPYMODE")) local_storage_copy_mode(arg);
    else if(!strcasecmp(line,"PROCESS")) {
        watch=false; power_managed=true; usb_retry_paused=true;
        if(release_disk()) local_storage_process_inbox();
    }
    else if(!strcasecmp(line,"HD")) local_storage_hd(arg);
    else if(!strcasecmp(line,"DEVICE")) local_storage_identity();
    else if(!strcasecmp(line,"META")) {
        if(arg && *arg) local_storage_metadata(arg,source_available);
        else LOG("Use META /usb/path/to/file.mp4 (or .jpg/.jpeg)");
    }
    else if(!strcasecmp(line,"POWER")) {
        int64_t now=esp_timer_get_time()/1000;
        if(!arg || !*arg || !strcasecmp(arg,"STATUS")) power_status();
        else if(!strcasecmp(arg,"MODE MANUAL") || !strcasecmp(arg,"MODE GPIO")) {
            if(soak_mode || power_cycle_busy()) { LOG("Stop SOAK and finish/cancel active collection before changing power mode"); return; }
            if(!release_disk()) return;
            bool gpio=!strcasecmp(arg,"MODE GPIO");
            if(power_cycle_set_mode(gpio?POWER_GPIO:POWER_MANUAL)) {
                if(!gpio && power_gpio_ready) {
                    check(gpio_reset_pin(GPIO_NUM_21),"release GPIO21"); power_gpio_ready=false;
                }
                power_managed=true; usb_retry_paused=true; watch=false;
            }
        } else if(!strcasecmp(arg,"CANCEL")) power_cycle_cancel();
        else if(!strcasecmp(arg,"OFF") || !strcasecmp(arg,"ON")) {
            if(!power_cycle_confirm(!strcasecmp(arg,"ON"),now)) LOG("POWER confirmation not expected in phase %s; use POWER STATUS",power_cycle_phase_name());
        } else LOG("POWER STATUS | MODE MANUAL | MODE GPIO | OFF | ON | CANCEL");
    }
    else if(!strcasecmp(line,"COLLECT")) {
        if(soak_mode) { LOG("Use SOAK NOW, or SOAK OFF before a one-shot COLLECT"); return; }
        if(!arg || !*arg || !strcasecmp(arg,"PLAN")) start_collection(JOB_PLAN);
        else if(!strcasecmp(arg,"COPY")) start_collection(JOB_COPY);
        else LOG("COLLECT PLAN (metadata only) | COLLECT COPY (verified local archive)");
    }
    else if (!strcasecmp(line,"SOAK")) soak_command(arg);
    else if (!strcasecmp(line,"PLAN")) {
        if (arg && !strcasecmp(arg,"RESET")) collection_reset();
        else if (arg && *arg) LOG("Use PLAN or PLAN RESET");
        else { bool ok=run_plan(); collection_finished(ok,esp_timer_get_time()/1000); }
    }
    else if (!strcasecmp(line,"SCHEDULE")) {
        if(soak_mode) { LOG("SOAK owns collection timing; use SOAK OFF before SCHEDULE"); return; }
        if(collection_command(arg,esp_timer_get_time()/1000)) {
            start_collection(JOB_SCHEDULE);
        }
    }
    else if (!strcasecmp(line,"RETRY")) {
        // Explicit diagnostics opt back into the existing bounded retry window.
        if(!soak_mode) power_managed=false;
        usb_attempts=0; usb_retry_paused=false;
        if (address && !removed && release_disk()) acquisition_result(attach(address,true));
        else LOG("RETRY needs connected USB device; does not switch VBUS");
    }
    else if (!strcasecmp(line,"SDMOUNT")) local_storage_mount();
    else if (!strcasecmp(line,"SDSTATUS")) local_storage_status();
    else if (!strcasecmp(line,"SDSCAN")) local_storage_scan();
    else if (!strcasecmp(line,"COPY") || !strcasecmp(line,"COPYALL")) {
        if(soak_mode) LOG("Use SOAK NOW, or SOAK OFF before a one-shot copy");
        else if(!strcasecmp(line,"COPY") && (!arg || !*arg || (strcmp(arg,"/usb") && strncmp(arg,"/usb/",5)) ||
                strstr(arg,"..") || strchr(arg,'\\') || strchr(arg,':'))) LOG("Use COPY /usb/path/to/file or COPYALL");
        else {
            start_collection(JOB_COPY);
            if(job==JOB_COPY && arg && !strcasecmp(line,"COPY")) snprintf(job_source,sizeof(job_source),"%s",arg);
        }
    }
    else if (!strcasecmp(line,"STATUS")) { status(); power_status(); }
    else if (!strcasecmp(line,"SCAN")) { if(vfs&&!removed) { scan("/usb",0); LOG("Storage scan %s", removed ? "interrupted" : "complete"); } else LOG("No mounted storage"); }
    else if (!strcasecmp(line,"READ")) read_file(arg&&*arg?arg:first_jpeg);
    else if (!strcasecmp(line,"WATCH")) { watch=arg?strcasecmp(arg,"OFF")!=0:!watch; status(); }
    else if (!strcasecmp(line,"RESET")) { if(disk&&!removed) check(msc_host_reset_recovery(disk),"MSC BOT reset recovery (no VBUS cycle)"); else LOG("No MSC device for reset"); }
    else if (!strcasecmp(line,"REINIT")) { LOG("Releasing storage then rebooting to reinitialize USB host"); if(release_disk()) { fflush(stdout); esp_restart(); } }
    else if (*line) LOG("Unknown command; use HELP");
}
void app_main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    int input_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (input_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK);
    LOG("StoneLink USB diagnostics; timestamps are boot milliseconds; external supply power workflow available");
    LOG("BOOT version=0.8.0 reset_reason=%d",esp_reset_reason());
    local_storage_identity();
    local_storage_copy_mode("STATUS");
    power_cycle_init(power_drive,power_notice);
    power_status();
    soak_load();
    LOG("Local SD auto-mount deferred; use SDMOUNT for FAT32 or exFAT destination card");
    events=xQueueCreate(16,sizeof(msc_host_event_t));
    if (!events) { ESP_LOGE(TAG,"Cannot allocate event queue"); return; }
    usb_host_config_t host={.skip_phy_setup=false,.intr_flags=ESP_INTR_FLAG_LEVEL1};
    if(!check(usb_host_install(&host),"USB host install")) return;
    if(xTaskCreate(daemon_task,"usb_lib",4096,NULL,5,NULL)!=pdPASS) { check(usb_host_uninstall(),"host cleanup"); return; }
    usb_host_client_config_t client={.is_synchronous=false,.max_num_event_msg=10,.async={.client_event_callback=observer_event,.callback_arg=NULL}};
    if(!check(usb_host_client_register(&client,&observer),"observer install")) return;
    msc_host_driver_config_t msc={.create_backround_task=true,.task_priority=5,.stack_size=4096,.core_id=tskNO_AFFINITY,.callback=msc_event};
    if(!check(msc_host_install(&msc),"MSC driver install")) return;
    LOG("USB host initialized; waiting for device; type HELP");
    if(soak_mode) {
        usb_retry_paused=true; soak_next=esp_timer_get_time()/1000+30000;
        LOG("SOAK restored mode=%u interval_s=%" PRIu32 "; first cycle in 30 seconds",soak_mode,soak_seconds);
    }
    char line[512]; size_t used=0; bool overflow=false; int64_t last=0;
    for (;;) {
        esp_err_t e=usb_host_client_handle_events(observer,0); if(e!=ESP_ERR_TIMEOUT) check(e,"observer events");
        msc_host_event_t ev;
        while(xQueueReceive(events,&ev,0)==pdTRUE) {
            if(ev.event==MSC_DEVICE_CONNECTED) {
                LOG("MSC attached address=%u",ev.device.address); address=ev.device.address; removed=false; usb_attempts=0;
                usb_retry_paused=soak_mode!=0 || power_managed || power_cycle_busy();
                if(!usb_retry_paused) acquisition_result(attach(address,true));
            }
            else if(ev.event==MSC_DEVICE_DISCONNECTED && (!disk || ev.device.handle==disk)) { LOG("MSC disconnected"); if(release_disk())address=0; }
        }
        clearerr(stdin);
        int c;
        while((c=getchar())!=EOF) {
            if(c=='\r'||c=='\n') { if(!overflow) { line[used]=0; command(line); } else LOG("Command too long; discarded"); used=0;overflow=false; }
            else if(c==8||c==127) { if(used)used--; }
            else if(c>=32) { if(used<sizeof(line)-1)line[used++]=(char)c; else overflow=true; }
        }
        service_collection();
        int64_t now=esp_timer_get_time();
        if(soak_mode && !power_cycle_busy() && now/1000>=soak_next) { soak_run(); now=esp_timer_get_time(); }
        if(soak_mode && now/1000-soak_heartbeat>=60000) { soak_status(); soak_heartbeat=now/1000; }
        if(!soak_mode && !power_cycle_busy() && collection_due(now/1000)) {
            start_collection(JOB_SCHEDULE);
            now=esp_timer_get_time();
        }
        if(!soak_mode&&!power_managed&&!power_cycle_busy()&&address&&!removed&&!vfs&&!usb_retry_paused&&usb_attempts<USB_MAX_ATTEMPTS&&now/1000>=usb_retry_after) {
            if(release_disk()) acquisition_result(attach(address,true));
            else acquisition_result(false);
        }
        if(watch&&now-last>2000000) {
            status();
            // Serialized with all filesystem work; never run raw reads concurrently.
            if(disk&&!removed&&sector_size>=512&&sector_size<=sizeof(probe)) {
                bool ready=check(scsi_cmd_read10(disk,probe,0,1,sector_size), "WATCH sector 0 read");
                LOG("WATCH media_readable=%d",ready);
                if(!ready) { release_disk(); acquisition_result(false); }
            }
            last=now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
