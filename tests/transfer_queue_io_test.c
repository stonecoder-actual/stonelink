// Production queue I/O with in-memory files, deterministic identity double and
// simulated fsync/rename/target failures. No physical disk or radio is used.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "dirent.h"
#include "sys/stat.h"
#include "../src/capture_metadata.c"
#include "../src/archive_naming.c"
#include "../src/transfer_queue.c"

static struct { char path[768],data[4096]; size_t length; } files[20];
static unsigned file_count,verifications;
static bool fail_sync,fail_rename,target_valid=true;
static char logs[20000];
typedef struct { unsigned index; size_t position; bool error; } FakeFile;
void test_log(const char *format,...) {
    size_t n=strlen(logs); va_list args; va_start(args,format);
    vsnprintf(logs+n,sizeof(logs)-n,format,args); va_end(args);
    n=strlen(logs); if(n+1<sizeof(logs)) { logs[n]='\n'; logs[n+1]=0; }
}
static int find_file(const char *path) {
    for(unsigned i=0;i<file_count;i++) if(!strcmp(path,files[i].path)) return i;
    return -1;
}
static FakeFile *fake_open(const char *path,const char *mode) {
    int index=find_file(path);
    if(*mode=='w') {
        if(index<0) { assert(file_count<20); index=file_count++; strcpy(files[index].path,path); }
        files[index].length=0;
    }
    if(index<0) { errno=ENOENT; return NULL; }
    FakeFile *file=calloc(1,sizeof(*file)); assert(file); file->index=index; return file;
}
static size_t fake_read(void *out,size_t size,size_t count,FakeFile *file) {
    assert(size==1); size_t available=files[file->index].length-file->position;
    size_t n=count<available?count:available;
    memcpy(out,files[file->index].data+file->position,n); file->position+=n; return n;
}
static size_t fake_write(const void *in,size_t size,size_t count,FakeFile *file) {
    assert(size==1 && file->position+count<4096);
    memcpy(files[file->index].data+file->position,in,count); file->position+=count;
    files[file->index].length=file->position; return count;
}
static int fake_close(FakeFile *file) { free(file); return 0; }
static int fake_flush(FakeFile *file) { (void)file; return 0; }
static int fake_error(FakeFile *file) { return file->error; }
static int fake_fileno(FakeFile *file) { return file->index; }
int fsync(int descriptor) { (void)descriptor; return fail_sync?-1:0; }
int stat(const char *path,struct stat *info) {
    if(!strcmp(path,"/queue")) { *info=(struct stat){1,0,0}; return 0; }
    int index=find_file(path);
    if(index<0) { errno=ENOENT; return -1; }
    *info=(struct stat){2,files[index].length,0}; return 0;
}
int rename(const char *from,const char *to) {
    if(fail_rename) { errno=EIO; return -1; }
    int index=find_file(from);
    if(index<0) { errno=ENOENT; return -1; }
    if(find_file(to)>=0) { errno=EEXIST; return -1; }
    strcpy(files[index].path,to); return 0;
}
DIR *opendir(const char *path) {
    if(strcmp(path,"/queue")) { errno=ENOENT; return NULL; }
    DIR *dir=calloc(1,sizeof(*dir)); assert(dir); return dir;
}
struct dirent *readdir(DIR *dir) {
    if(dir->next==file_count) return NULL;
    strcpy(dir->entry.d_name,strrchr(files[dir->next++].path,'/')+1); return &dir->entry;
}
int closedir(DIR *dir) { free(dir); return 0; }
int mbedtls_sha256(const unsigned char *data,size_t length,unsigned char output[32],int is224) {
    assert(!is224); uint32_t value=2166136261u;
    for(size_t i=0;i<length;i++) value=(value^data[i])*16777619u;
    for(unsigned i=0;i<32;i++) { value=value*1664525u+1013904223u; output[i]=value>>24; }
    return 0;
}
#define FILE FakeFile
#define fopen fake_open
#define fread fake_read
#define fwrite fake_write
#define fclose fake_close
#define fflush fake_flush
#define ferror fake_error
#define fileno fake_fileno
#include "../src/transfer_queue_io.c"
#undef FILE
#undef fopen
#undef fread
#undef fwrite
#undef fclose
#undef fflush
#undef ferror
#undef fileno

static const char *prefix="ESP-AABBCCDDEEFF";
static bool resolve(const char *reference,transfer_job_t *job) {
    unsigned index=!strcmp(reference,"FIRST")?0:!strcmp(reference,"SECOND")?1:2;
    strcpy(job->prefix,prefix); memset(job->capture_id,'a'+index,64); job->capture_id[64]=0;
    memset(job->sha256,'1'+index,64); job->sha256[64]=0; job->bytes=100+index;
    capture_metadata_t capture={"2026-11-26 14:30:05","test"};
    assert(archive_media_path(job->path,sizeof(job->path),"/sd/stonelink",prefix,"file.mp4",&capture,job->sha256,job->capture_id,"HD"));
    char sd[768]; assert(archive_quality_path(sd,sizeof(sd),job->path,prefix,"SD"));
    strcpy(job->requested_filename,strrchr(sd,'/')+1); return true;
}
static bool verify(const transfer_job_t *job) { (void)job; verifications++; return target_valid; }
static void run(const char *command) { logs[0]=0; transfer_queue_command(command,"/queue",prefix,resolve,verify); }
static void read_id(unsigned index,char out[65]) { memcpy(out,strrchr(files[index].path,'/')+1,64); out[64]=0; }
int main(void) {
    run("LIST"); assert(strstr(logs,"pending=0"));
    run("REQUEST FIRST"); assert(strstr(logs,"HD QUEUED") && file_count==1);
    char first[65],second[65],command[100]; read_id(0,first);
    run("REQUEST FIRST"); assert(strstr(logs,"ALREADY QUEUED") && file_count==1);
    run("REQUEST SECOND"); assert(strstr(logs,"HD QUEUED") && file_count==2); read_id(1,second);
    run("NEXT"); assert(strstr(logs,"HD READY") && strstr(logs,first) && strstr(logs,"NOT TRANSMITTED"));
    memset(text,0,sizeof(text)); memset(readback,0,sizeof(readback)); // Reboot: no RAM queue state needed.
    run("LIST"); assert(strstr(logs,"pending=2"));
    snprintf(command,sizeof(command),"CANCEL %s",first); run(command); assert(strstr(logs,"HD CANCELED"));
    run("NEXT"); assert(strstr(logs,second) && strstr(logs,"HD READY"));
    snprintf(command,sizeof(command),"RETRY %s",first); run(command); assert(strstr(logs,"HD REQUEUED"));
    target_valid=false; run("NEXT"); assert(strstr(logs,"HD BLOCKED") && file_count==2);
    target_valid=true; run("NEXT"); assert(strstr(logs,first) && strstr(logs,"HD READY"));
    fail_rename=true; snprintf(command,sizeof(command),"CANCEL %s",first); run(command); assert(strstr(logs,"rename failed"));
    fail_rename=false; run("LIST"); assert(strstr(logs,"pending=2"));
    fail_sync=true; run("REQUEST THIRD"); assert(strstr(logs,"publication failed"));
    run("LIST"); assert(strstr(logs,"pending=2")); // A partial staging file is never a queued request.
    fail_sync=false; run("REQUEST THIRD"); assert(strstr(logs,"HD QUEUED"));
    run("LIST"); assert(strstr(logs,"pending=3"));
    char saved[4096]; size_t saved_length=files[0].length; memcpy(saved,files[0].data,saved_length);
    strcpy(files[0].data,"{"); files[0].length=1;
    run("LIST"); assert(strstr(logs,"scan failed")); // No partial index or successful enqueue.
    memcpy(files[0].data,saved,saved_length); files[0].length=saved_length;
    run("NEXT"); assert(strstr(logs,first) && strstr(logs,"HD READY"));
    puts("PASS persistent queue I/O: FIFO, duplicate requests, reboot, cancel/retry, blocked target, fsync/rename/corruption faults");
    return 0;
}
