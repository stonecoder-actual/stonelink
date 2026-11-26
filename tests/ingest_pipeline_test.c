// Runs the unmodified algorithmic body extracted from production local_storage.c,
// plus actual ingest_store/media_group code. Only mount/MAC, filesystem, time and
// SHA primitives are mocked. CRC uses Espressif's real host ROM implementation.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include "dirent.h"
#include "sys/stat.h"
#include "../src/local_storage.h"
#include "../src/archive_naming.h"
#include "../src/media_group.h"
#include "../src/ingest_store.h"
#include "../src/media_io.h"
#include "../src/media_filter.h"
#include "../src/transfer_queue.h"
#include "mbedtls/sha256.h"
#include "esp_log.h"

static struct { char path[768]; uint8_t *data; size_t length,capacity; int64_t mtime; bool active,directory; } nodes[512];
static unsigned node_count,usb_opens,usb_seeks,normalizing_while_usb;
static uint64_t usb_bytes,old_local_bytes,cut_after;
static bool usb_owned=true;
static char opened[30][768],logs[60000];
static int64_t clock_us;
static unsigned delays;
int64_t esp_timer_get_time(void) { clock_us+=100; return clock_us; }
void vTaskDelay(unsigned ticks) { assert(ticks==1); delays++; clock_us+=10000; }
void test_log(const char *format,...) {
    size_t n=strlen(logs); va_list args; va_start(args,format); vsnprintf(logs+n,sizeof(logs)-n,format,args); va_end(args);
    n=strlen(logs); if(n+1<sizeof(logs)) { logs[n]='\n'; logs[n+1]=0; }
}
static bool source_path(const char *path) { return !strncmp(path,"/usb",4) && (path[4]=='/' || !path[4]); }
static int find_node(const char *path) {
    for(unsigned i=0;i<node_count;i++) if(nodes[i].active && !strcmp(nodes[i].path,path)) return i;
    return -1;
}
static int make_node(const char *path,bool directory) {
    int index=find_node(path); if(index>=0) return index;
    assert(node_count<512); index=node_count++; strcpy(nodes[index].path,path); nodes[index].active=true; nodes[index].directory=directory; return index;
}
static void put_bytes(int index,const void *data,size_t n) {
    if(n>nodes[index].capacity) { nodes[index].data=realloc(nodes[index].data,n); assert(nodes[index].data); nodes[index].capacity=n; }
    memcpy(nodes[index].data,data,n); nodes[index].length=n;
}
typedef struct { unsigned index; size_t position; bool error; } FakeFile;
static FakeFile *fake_open(const char *path,const char *mode) {
    if(source_path(path)) { assert(usb_owned && *mode=='r'); assert(usb_opens<30); strcpy(opened[usb_opens++],path); }
    int index=find_node(path);
    if(*mode=='w' || *mode=='a') {
        assert(!source_path(path));
        if(index<0) index=make_node(path,false);
        if(*mode=='w') nodes[index].length=0;
    }
    if(index<0 || nodes[index].directory) { errno=ENOENT; return NULL; }
    FakeFile *file=calloc(1,sizeof(*file)); assert(file); file->index=index;
    if(*mode=='a') file->position=nodes[index].length;
    return file;
}
static size_t fake_read(void *out,size_t size,size_t count,FakeFile *file) {
    assert(size==1); unsigned index=file->index; size_t available=nodes[index].length-file->position;
    size_t n=count<available?count:available;
    if(source_path(nodes[index].path)) { assert(usb_owned); usb_bytes+=n; }
    if(strstr(nodes[index].path,"_PHOTO_") || strstr(nodes[index].path,"_VIDEO_")) old_local_bytes+=n;
    memcpy(out,nodes[index].data+file->position,n); file->position+=n; return n;
}
static size_t fake_write(const void *in,size_t size,size_t count,FakeFile *file) {
    assert(size==1); unsigned index=file->index; assert(!source_path(nodes[index].path));
    size_t end=file->position+count;
    if(end>nodes[index].capacity) { nodes[index].data=realloc(nodes[index].data,end); assert(nodes[index].data); nodes[index].capacity=end; }
    memcpy(nodes[index].data+file->position,in,count); file->position=end;
    if(end>nodes[index].length) nodes[index].length=end;
    return count;
}
static int fake_seek(FakeFile *file,long offset,int origin) {
    if(source_path(nodes[file->index].path)) { assert(usb_owned); usb_seeks++; }
    int64_t next=offset+(origin==SEEK_END?(int64_t)nodes[file->index].length:origin==SEEK_CUR?(int64_t)file->position:0);
    if(next<0 || next>(int64_t)nodes[file->index].length) return -1;
    file->position=next; return 0;
}
static int fake_getc(FakeFile *file) { uint8_t c; return fake_read(&c,1,1,file)==1?c:EOF; }
static int fake_close(FakeFile *file) { free(file); return 0; }
static int fake_flush(FakeFile *file) { (void)file; return 0; }
static int fake_error(FakeFile *file) { return file->error; }
static int fake_fileno(FakeFile *file) { return file->index; }
static int fake_puts(const char *text,FakeFile *file) { return fake_write(text,1,strlen(text),file)==strlen(text)?0:EOF; }
static int fake_printf(FakeFile *file,const char *format,...) {
    char text[2048]; va_list args; va_start(args,format); int n=vsnprintf(text,sizeof(text),format,args); va_end(args);
    assert(n>=0 && n<sizeof(text)); return fake_write(text,1,n,file)==(size_t)n?n:-1;
}
int fsync(int descriptor) { (void)descriptor; return 0; }
int stat(const char *path,struct stat *info) {
    if(source_path(path)) assert(usb_owned);
    int index=find_node(path); if(index<0) { errno=ENOENT; return -1; }
    *info=(struct stat){nodes[index].directory?1:2,nodes[index].length,nodes[index].mtime}; return 0;
}
int mkdir(const char *path,unsigned mode) { (void)mode; assert(!source_path(path)); if(find_node(path)>=0) { errno=EEXIST; return -1; } make_node(path,true); return 0; }
int rename(const char *from,const char *to) {
    assert(!source_path(from) && !source_path(to)); int index=find_node(from);
    if(index<0) { errno=ENOENT; return -1; } if(find_node(to)>=0) { errno=EEXIST; return -1; }
    if(usb_owned && (strstr(to,"_PHOTO_") || strstr(to,"_VIDEO_"))) normalizing_while_usb++;
    strcpy(nodes[index].path,to); return 0;
}
int unlink(const char *path) { assert(!source_path(path)); int index=find_node(path); if(index<0) { errno=ENOENT; return -1; } nodes[index].active=false; return 0; }
DIR *opendir(const char *path) {
    struct stat info; if(stat(path,&info) || !S_ISDIR(info.st_mode)) return NULL;
    assert(strlen(path)<256); DIR *dir=calloc(1,sizeof(*dir)); assert(dir); strcpy(dir->path,path); return dir;
}
struct dirent *readdir(DIR *dir) {
    size_t prefix=strlen(dir->path);
    while(dir->next<node_count) {
        unsigned i=dir->next++; if(!nodes[i].active) continue;
        if(!strncmp(nodes[i].path,dir->path,prefix) && nodes[i].path[prefix]=='/' && !strchr(nodes[i].path+prefix+1,'/')) {
            strcpy(dir->entry.d_name,nodes[i].path+prefix+1); return &dir->entry;
        }
    }
    return NULL;
}
int closedir(DIR *dir) { free(dir); return 0; }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { memset(c,0,sizeof(*c)); }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int is224) { assert(!is224); c->value=2166136261u; c->bytes=0; return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *data,size_t n) { for(size_t i=0;i<n;i++) c->value=(c->value^data[i])*16777619u; c->bytes+=n; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char out[32]) { uint32_t v=c->value^(uint32_t)c->bytes; for(unsigned i=0;i<32;i++) { v=v*1664525u+1013904223u; out[i]=v>>24; } return 0; }
int mbedtls_sha256(const unsigned char *data,size_t n,unsigned char out[32],int is224) { mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c,is224); mbedtls_sha256_update(&c,data,n); return mbedtls_sha256_finish(&c,out); }
void transfer_queue_command(const char *args,const char *folder,const char *prefix,transfer_resolve_fn resolve,transfer_verify_fn verify) { (void)args;(void)folder;(void)prefix;(void)resolve;(void)verify; }
void local_storage_status(void) {}

#define FILE FakeFile
#define fopen fake_open
#define fread fake_read
#define fwrite fake_write
#define fclose fake_close
#define fflush fake_flush
#define ferror fake_error
#define fileno fake_fileno
#define fseek fake_seek
#define fgetc fake_getc
#define fputs fake_puts
#define fprintf fake_printf
#include "../src/capture_metadata.c"
#include "../src/archive_naming.c"
#define TAG group_tag
#include "../src/media_group.c"
#undef TAG
#include "../src/ingest_store.c"
#include "../src/media_io.c"

// Hardware adapter only. The algorithm body below is copied verbatim by the
// runner from static joined() onward; no production logic is rewritten here.
#define PATH_SIZE 768
#define STORE "/sd/stonelink"
static const char *TAG="local_sd";
static bool card=true,copy_verify_mode;
static uint8_t buffer[8192];
static bool (*source_available)(void);
static unsigned received,fast_skipped,staged_skipped,ignored,planned,recovery_planned;
static char device_prefix[17]="ESP-AABBCCDDEEFF";
static media_group_result_t active_group;
static bool ensure_identity(void) { return true; }
#include "../.pio/ingest_under_test.inc"
#undef FILE
#undef fopen
#undef fread
#undef fwrite
#undef fclose
#undef fflush
#undef ferror
#undef fileno
#undef fseek
#undef fgetc
#undef fputs
#undef fprintf

static bool available(void) { return usb_owned && (!cut_after || usb_bytes<cut_after); }
static void reset_metrics(void) { usb_bytes=old_local_bytes=usb_opens=usb_seeks=normalizing_while_usb=0; logs[0]=0; cut_after=0; }
static void source_jpeg(const char *path,size_t size,char second,int64_t mtime) {
    assert(size>=110); uint8_t *data=calloc(1,size); assert(data);
    data[0]=255; data[1]=216; data[2]=255; data[3]=225; data[4]=0; data[5]=104; memcpy(data+6,"Exif\0\0",6);
    uint8_t *t=data+12; t[0]=t[1]='I'; t[2]=42; t[4]=8; t[8]=1;
    t[10]=0x69; t[11]=0x87; t[12]=4; t[14]=1; t[18]=26;
    t[26]=1; t[28]=3; t[29]=0x90; t[30]=2; t[32]=20; t[36]=44;
    memcpy(t+44,"2026:11:26 14:30:05",20); t[44+18]=second;
    for(size_t i=108;i<size;i++) data[i]=(uint8_t)(i*17);
    int index=make_node(path,false); put_bytes(index,data,size); nodes[index].mtime=mtime; free(data);
}
static unsigned normalized_files(void) { unsigned n=0; for(unsigned i=0;i<node_count;i++) if(nodes[i].active && (strstr(nodes[i].path,"_PHOTO_")||strstr(nodes[i].path,"_VIDEO_"))) n++; return n; }
int main(void) {
    make_node("/sd",true); make_node("/sd/stonelink",true); make_node("/usb",true); make_node("/usb/DCIM",true); make_node("/usb/other",true);
    source_jpeg("/usb/DCIM/ORIGINAL.JPG",256,'5',1000); source_jpeg("/usb/other/preview.jpeg",128,'5',1000);
    int junk=make_node("/usb/DCIM/sync.dat",false); put_bytes(junk,"junk",4);
    assert(local_storage_receive("/usb",available));
    assert(usb_bytes==384 && usb_opens==2 && !usb_seeks && !normalizing_while_usb && !normalized_files());
    ingest_record_t saved,damaged;
    assert(ingest_source_lookup(STORE,device_prefix,"/usb/DCIM/ORIGINAL.JPG",256,1000,&saved));
    damaged=saved; damaged.bytes++;
    assert(!ingest_record_valid(&damaged,device_prefix)); // Torn/corrupt index cannot authorize a skip.
    assert(!ingest_record_valid(&saved,"ESP-112233445566"));
    assert(!ingest_source_lookup(STORE,device_prefix,"/usb/DCIM/ORIGINAL.JPG",256,1001,&damaged));
    // Simulate restarting acquisition before the backend has run. Durable
    // receipts avoid re-reading complete files even without a normalized archive.
    reset_metrics();
    assert(local_storage_receive("/usb",available));
    assert(!usb_bytes && !usb_opens && staged_skipped==2 && !normalized_files());
    usb_owned=false;
    assert(local_storage_process_inbox()); assert(normalized_files()==2);
    bool hd=false,sd=false;
    for(unsigned i=0;i<node_count;i++) if(nodes[i].active && strstr(nodes[i].path,"_PHOTO_")) { if(strstr(nodes[i].path,"_HD_")) { hd=true; assert(nodes[i].length==256); } else { sd=true; assert(nodes[i].length==128); } }
    assert(hd&&sd); // Metadata, payload readback and flag renames happened offline.

    reset_metrics(); usb_owned=true;
    assert(local_storage_receive("/usb",available));
    assert(!usb_bytes && !usb_opens && !usb_seeks && !old_local_bytes && fast_skipped==2 && !normalizing_while_usb);
    usb_owned=false; assert(local_storage_process_inbox());

    source_jpeg("/usb/DCIM/NEW.JPG",200,'6',1001); reset_metrics(); usb_owned=true;
    assert(local_storage_receive("/usb",available)); assert(usb_bytes==200 && usb_opens==1 && !old_local_bytes && !normalizing_while_usb);
    assert(!strcmp(opened[0],"/usb/DCIM/NEW.JPG"));
    usb_owned=false; assert(local_storage_process_inbox()); assert(normalized_files()==3);

    source_jpeg("/usb/DCIM/LATEST.JPG",160,'7',1002); local_storage_copy_mode("VERIFY"); reset_metrics(); usb_owned=true;
    assert(local_storage_receive("/usb",available)); assert(!strcmp(opened[0],"/usb/DCIM/LATEST.JPG"));
    assert(usb_bytes==744 && usb_opens==4 && !normalizing_while_usb);
    usb_owned=false; assert(local_storage_process_inbox()); assert(normalized_files()==4);

    // The explicit FAST tradeoff: same source name/size/time can hide changed
    // bytes. VERIFY must detect/archive that content and disable ambiguous cache.
    int original=find_node("/usb/DCIM/ORIGINAL.JPG"); nodes[original].data[200]^=1;
    local_storage_copy_mode("FAST"); reset_metrics(); usb_owned=true;
    assert(local_storage_receive("/usb",available)); assert(!usb_bytes && fast_skipped==4);
    usb_owned=false; assert(local_storage_process_inbox());
    local_storage_copy_mode("VERIFY"); reset_metrics(); usb_owned=true;
    assert(local_storage_receive("/usb",available));
    usb_owned=false; assert(local_storage_process_inbox()); assert(normalized_files()==5);

    // An interruption on a later file retains the already received new file.
    local_storage_copy_mode("FAST"); source_jpeg("/usb/DCIM/GOOD.JPG",1000,'8',1003); source_jpeg("/usb/DCIM/INTERRUPTED.JPG",20000,'9',1004);
    reset_metrics(); usb_owned=true; cut_after=256+1000+8192+1; // conflict source 256 is deliberately re-read.
    assert(!local_storage_receive("/usb",available));
    usb_owned=false; assert(local_storage_process_inbox()); assert(normalized_files()==6);
    // All backend stat/open calls to /usb would have asserted above.
    puts("PASS actual ingest pipeline: single USB pass, zero old payload reads on FAST, new-first VERIFY, offline normalization, cache collision and interrupted acquisition");
    for(unsigned i=0;i<node_count;i++) free(nodes[i].data);
    return 0;
}
