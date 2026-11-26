#include "transfer_queue.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#define PATH_SIZE 768
static const char *TAG="transfer";
static char text[4096],readback[4096]; // Main task only; no whole-queue RAM index.
static bool request_identity(const char *capture,const char *sha,char out[65]) {
    char key[160]; uint8_t digest[32]; static const char hex[]="0123456789abcdef";
    int n=snprintf(key,sizeof(key),"HD-v1|%s|%s",capture,sha);
    if(n<0 || n>=sizeof(key) || mbedtls_sha256((const unsigned char *)key,n,digest,0)) return false;
    for(unsigned i=0;i<32;i++) { out[i*2]=hex[digest[i]>>4]; out[i*2+1]=hex[digest[i]&15]; }
    out[64]=0; return true;
}
static bool path_for(char *path,const char *folder,const char *id,bool canceled) {
    if(!archive_hex_id(id)) return false;
    int n=snprintf(path,PATH_SIZE,"%s/%s.%s.json",folder,id,canceled?"canceled":"pending");
    return n>=0 && n<PATH_SIZE;
}
static bool read_job(const char *path,const char *prefix,const char *id,transfer_job_t *job) {
    FILE *file=fopen(path,"rb"); if(!file) return false;
    size_t n=fread(text,1,sizeof(text)-1,file);
    bool ok=!ferror(file) && n<sizeof(text)-1;
    if(fclose(file)) ok=false;
    text[n]=0;
    if(!ok || !transfer_job_parse(text,job,prefix,id)) return false;
    char calculated[65];
    return request_identity(job->capture_id,job->sha256,calculated) && !strcmp(calculated,id);
}
typedef struct { uint32_t maximum,pending,canceled; bool have_next; transfer_job_t next; } queue_scan_t;
static bool scan_queue(const char *folder,const char *prefix,queue_scan_t *result,bool list) {
    memset(result,0,sizeof(*result)); DIR *dir=opendir(folder); if(!dir) return false;
    bool ok=true; unsigned entries=0;
    for(;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if(!entry) { if(errno) ok=false; break; }
        if(++entries>10000) { ok=false; break; }
        size_t n=strlen(entry->d_name);
        if(n<=64 || entry->d_name[0]=='.') continue;
        bool canceled=!strcmp(entry->d_name+64,".canceled.json");
        if(!canceled && strcmp(entry->d_name+64,".pending.json")) continue;
        char id[65],path[PATH_SIZE]; transfer_job_t job;
        memcpy(id,entry->d_name,64); id[64]=0;
        if(!path_for(path,folder,id,canceled) || !read_job(path,prefix,id,&job)) { ok=false; break; }
        if(job.sequence>result->maximum) result->maximum=job.sequence;
        if(canceled) result->canceled++; else result->pending++;
        if(!canceled && (!result->have_next || job.sequence<result->next.sequence ||
            (job.sequence==result->next.sequence && strcmp(id,result->next.request_id)<0))) {
            result->next=job; result->have_next=true;
        }
        if(list) ESP_LOGI(TAG,"HD JOB request=%s state=%s sequence=%" PRIu32 " capture=%s filename=%s",
                         id,canceled?"CANCELED":"PENDING",job.sequence,job.capture_id,strrchr(job.path,'/')+1);
    }
    if(closedir(dir)) ok=false;
    if(!ok) ESP_LOGE(TAG,"HD queue scan failed; no pending jobs removed or transmitted");
    return ok;
}
static bool publish(const char *folder,const char *path,const transfer_job_t *job) {
    if(!transfer_job_json(text,sizeof(text),job)) return false;
    size_t length=strlen(text); char temporary[PATH_SIZE];
    if(snprintf(temporary,sizeof(temporary),"%s/.job.part",folder)>=sizeof(temporary)) return false;
    struct stat existing;
    if(!stat(path,&existing) || errno!=ENOENT) return false;
    FILE *file=fopen(temporary,"wb"); if(!file) return false;
    bool ok=fwrite(text,1,length,file)==length;
    if(fflush(file)) ok=false;
    if(fsync(fileno(file))) ok=false;
    if(fclose(file)) ok=false;
    if(ok) {
        file=fopen(temporary,"rb");
        if(!file) ok=false;
        else {
            size_t n=fread(readback,1,length+1,file);
            if(ferror(file) || n!=length || memcmp(text,readback,length)) ok=false;
            if(fclose(file)) ok=false;
        }
    }
    if(ok && rename(temporary,path)) ok=false;
    return ok;
}
void transfer_queue_command(const char *args,const char *folder,const char *prefix,
                            transfer_resolve_fn resolve,transfer_verify_fn verify) {
    const char *arg=args?args:"LIST";
    if(!*arg || !strcasecmp(arg,"LIST") || !strcasecmp(arg,"NEXT")) {
        queue_scan_t scan; if(!scan_queue(folder,prefix,&scan,strcasecmp(arg,"NEXT")!=0)) return;
        ESP_LOGI(TAG,"HD QUEUE pending=%" PRIu32 " canceled=%" PRIu32 "; local queue only; no transport attached",scan.pending,scan.canceled);
        if(!strcasecmp(arg,"NEXT") && scan.have_next) {
            if(verify(&scan.next)) ESP_LOGI(TAG,"HD READY request=%s capture=%s bytes=%" PRIu64 " SHA256=%s filename=%s; NOT TRANSMITTED",
                                          scan.next.request_id,scan.next.capture_id,scan.next.bytes,scan.next.sha256,strrchr(scan.next.path,'/')+1);
            else ESP_LOGW(TAG,"HD BLOCKED request=%s; missing/changed HD or checksum failure; request remains pending",scan.next.request_id);
        }
    } else if(!strncasecmp(arg,"REQUEST ",8)) {
        transfer_job_t job={0}; queue_scan_t scan;
        if(!resolve(arg+8,&job) || !request_identity(job.capture_id,job.sha256,job.request_id) ||
           !scan_queue(folder,prefix,&scan,false)) return;
        char pending[PATH_SIZE],canceled[PATH_SIZE]; struct stat info;
        if(!path_for(pending,folder,job.request_id,false) || !path_for(canceled,folder,job.request_id,true)) return;
        if(!stat(pending,&info)) { ESP_LOGI(TAG,"HD ALREADY QUEUED request=%s",job.request_id); return; }
        if(errno!=ENOENT) return;
        if(!stat(canceled,&info)) { ESP_LOGI(TAG,"HD previously canceled request=%s; use HD RETRY request_id",job.request_id); return; }
        if(errno!=ENOENT || scan.maximum==UINT32_MAX) return;
        job.sequence=scan.maximum+1;
        if(!verify(&job)) { ESP_LOGE(TAG,"HD target could not be verified; not queued"); return; }
        if(publish(folder,pending,&job)) ESP_LOGI(TAG,"HD QUEUED request=%s capture=%s sequence=%" PRIu32 " filename=%s",
                                               job.request_id,job.capture_id,job.sequence,strrchr(job.path,'/')+1);
        else ESP_LOGE(TAG,"HD queue publication failed; no successful enqueue recorded");
    } else if(!strncasecmp(arg,"CANCEL ",7) || !strncasecmp(arg,"RETRY ",6)) {
        bool retry=!strncasecmp(arg,"RETRY ",6); const char *id=arg+(retry?6:7);
        char from[PATH_SIZE],to[PATH_SIZE]; transfer_job_t job; struct stat info;
        if(!path_for(from,folder,id,retry) || !path_for(to,folder,id,!retry) || !read_job(from,prefix,id,&job)) {
            ESP_LOGE(TAG,"HD request not found/invalid; use the request ID from HD LIST"); return;
        }
        if(!stat(to,&info) || errno!=ENOENT || (retry && !verify(&job))) {
            ESP_LOGE(TAG,"HD state conflict or changed target; no state change"); return;
        }
        if(rename(from,to)) ESP_LOGE(TAG,"HD state rename failed errno=%d",errno);
        else ESP_LOGI(TAG,"HD %s request=%s; media preserved",retry?"REQUEUED":"CANCELED",id);
    } else ESP_LOGI(TAG,"HD LIST | NEXT | REQUEST archived_filename_or_capture_id | CANCEL request_id | RETRY request_id");
}
