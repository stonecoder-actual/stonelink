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

// ESP-IDF 5.4 FatFS is writable. Linker --wrap intercepts all FatFS sector writes.
// Do not call raw MSC write APIs; this barrier protects the filesystem path.
DRESULT __wrap_ff_disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    ESP_LOGE("write_guard", "Blocked FatFS disk write");
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
    while (!removed && (n=fread(buf,1,sizeof(buf),f))) {
        total+=n;
        for (size_t i=0;i<n;i++) { crc ^= buf[i]; for(int b=0;b<8;b++) crc=(crc>>1)^((crc&1)?0xedb88320:0); }
        vTaskDelay(1);
    }
    bool ok = !removed && !ferror(f) && total == (uint64_t)st.st_size;
    fclose(f);
    if (ok) LOG("READ %s bytes=%" PRIu64 " CRC32/ISO-HDLC=%08" PRIx32, path,total,crc^0xffffffff);
    else ESP_LOGE(TAG,"READ incomplete %s bytes=%" PRIu64,path,total);
}
static void attach(uint8_t addr) {
    if (disk) return;
    removed=false; address=addr;
    if (!check(msc_host_install_device(addr,&disk),"MSC readiness/install")) return;
    msc_host_device_info_t info = {0};
    if (check(msc_host_get_device_info(disk,&info),"capacity")) LOG("Storage ready VID=%04x PID=%04x sectors=%" PRIu32 " sector_size=%" PRIu32 " capacity=%" PRIu64,info.idVendor,info.idProduct,info.sector_count,info.sector_size,(uint64_t)info.sector_count*info.sector_size);
    sector_size=info.sector_size;
    esp_vfs_fat_mount_config_t mount = {.format_if_mount_failed=false,.max_files=4,.allocation_unit_size=0};
    if (!check(msc_host_vfs_register(disk,"/usb",&mount,&vfs),"read-only FAT mount")) return;
    LOG("FAT filesystem mounted; disk_write barrier enabled; FF_FS_READONLY=%d",FF_FS_READONLY);
    scan("/usb",0); LOG("Storage scan %s", removed ? "interrupted" : "complete");
    if (first_jpeg[0]) read_file(first_jpeg);
    else LOG("No JPEG discovered");
}
static void status(void) { LOG("STATUS address=%u installed=%d mounted=%d removed=%d WATCH=%d uptime_ms=%lld",address,disk!=NULL,vfs!=NULL,removed,watch,(long long)(esp_timer_get_time()/1000)); }
static void command(char *line) {
    char *arg=strchr(line,' '); if(arg) { *arg++=0; while(*arg==' ')arg++; }
    if (!strcasecmp(line,"HELP")) LOG("HELP | STATUS | SCAN | READ /usb/path (or first JPEG) | RESET | REINIT | WATCH [ON|OFF]");
    else if (!strcasecmp(line,"STATUS")) status();
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
    LOG("StoneLink USB diagnostics; timestamps are boot milliseconds; no VBUS switching");
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
    char line[512]; size_t used=0; bool overflow=false; int64_t last=0,retry=0;
    for (;;) {
        esp_err_t e=usb_host_client_handle_events(observer,0); if(e!=ESP_ERR_TIMEOUT) check(e,"observer events");
        msc_host_event_t ev;
        while(xQueueReceive(events,&ev,0)==pdTRUE) {
            if(ev.event==MSC_DEVICE_CONNECTED) { LOG("MSC attached address=%u",ev.device.address); address=ev.device.address; attach(address); retry=esp_timer_get_time(); }
            else if(ev.event==MSC_DEVICE_DISCONNECTED && (!disk || ev.device.handle==disk)) { LOG("MSC disconnected"); if(release_disk())address=0; }
        }
        clearerr(stdin);
        int c;
        while((c=getchar())!=EOF) {
            if(c=='\r'||c=='\n') { if(!overflow) { line[used]=0; command(line); } else LOG("Command too long; discarded"); used=0;overflow=false; }
            else if(c==8||c==127) { if(used)used--; }
            else if(c>=32) { if(used<sizeof(line)-1)line[used++]=(char)c; else overflow=true; }
        }
        int64_t now=esp_timer_get_time();
        if(address&&!removed&&!vfs&&now-retry>5000000) { if(release_disk())attach(address); retry=now; }
        if(watch&&now-last>2000000) {
            status();
            // Serialized with all filesystem work; never run raw reads concurrently.
            if(disk&&!removed&&sector_size>=512&&sector_size<=sizeof(probe)) {
                bool ready=check(scsi_cmd_read10(disk,probe,0,1,sector_size), "WATCH sector 0 read");
                LOG("WATCH media_readable=%d",ready);
                if(!ready) { release_disk(); retry=now; }
            }
            last=now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
