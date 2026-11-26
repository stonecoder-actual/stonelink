// Executes the production collection implementation against an in-memory,
// read-only source. No disks, ESP32, source writes or destination card needed.
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "dirent.h"
#include "sys/stat.h"

static struct { char path[256]; struct stat info; } nodes[300];
static unsigned node_count;
static bool ready=true;
static bool directory_error;
static char summary[1024];

void test_log(const char *format,...)
{
    char message[1024]; va_list args;
    va_start(args,format); vsnprintf(message,sizeof(message),format,args); va_end(args);
    if(strstr(message,"PLAN complete")) strcpy(summary,message);
}

int stat(const char *path,struct stat *out)
{
    for(unsigned i=0;i<node_count;i++) if(!strcmp(path,nodes[i].path)) { *out=nodes[i].info; return 0; }
    errno=ENOENT; return -1;
}

DIR *opendir(const char *path)
{
    struct stat info;
    if(stat(path,&info) || !S_ISDIR(info.st_mode)) { errno=ENOENT; return NULL; }
    DIR *dir=calloc(1,sizeof(*dir)); assert(dir); strcpy(dir->path,path); return dir;
}

struct dirent *readdir(DIR *dir)
{
    if(directory_error) { errno=EIO; return NULL; }
    size_t prefix=strlen(dir->path);
    while(dir->next<node_count) {
        const char *path=nodes[dir->next++].path;
        if(!strncmp(path,dir->path,prefix) && path[prefix]=='/' && !strchr(path+prefix+1,'/')) {
            strcpy(dir->entry.d_name,path+prefix+1); return &dir->entry;
        }
    }
    return NULL;
}
int closedir(DIR *dir) { free(dir); return 0; }

#include "../src/collection.c"

static bool available(void) { return ready; }
static void add(const char *path,unsigned mode,uint64_t size,int64_t mtime)
{
    assert(node_count<300);
    strcpy(nodes[node_count].path,path);
    nodes[node_count++].info=(struct stat){mode,size,mtime};
}
static void expect(const char *fragment) { assert(strstr(summary,fragment)); }

int main(void)
{
    // Exact deadlines, bounded retries, disabling, validated test intervals.
    assert(!collection_due(INT64_C(999999999)));
    assert(!collection_command("ON",1000));
    assert(!collection_due(21600999)); assert(collection_due(21601000));
    collection_finished(false,21601000);
    assert(!collection_due(21630999)); assert(collection_due(21631000));
    collection_finished(false,21631000);
    assert(collection_due(21661000));
    collection_finished(false,21661000);
    assert(!collection_due(21691000)); assert(collection_due(43261000));
    collection_command("OFF",0); assert(!collection_due(INT64_C(999999999)));
    assert(collection_command("NOW",0)); collection_finished(true,0);
    assert(!collection_due(INT64_C(999999999)));
    collection_command("EVERY 60",0); collection_command("ON",500);
    assert(!collection_due(60499)); assert(collection_due(60500));
    collection_command("EVERY 0",500); assert(collection_due(60500));
    collection_command("EVERY -1",500); assert(collection_due(60500));
    collection_command("EVERY 60junk",500); assert(collection_due(60500));
    collection_finished(true,61000); assert(!collection_due(120999)); assert(collection_due(121000));
    collection_command("OFF",0);

    // Known files plus a nonmedia file: first baseline, unchanged repeat.
    add("/usb/DCIM",1,0,0); add("/usb/DCIM/100MEDIA",1,0,0);
    add("/usb/DCIM/100MEDIA/A.JpEg",2,100,10);
    add("/usb/DCIM/100MEDIA/B.mP4",2,200,20);
    add("/usb/DCIM/sync0.dat",2,46,20);
    add("/usb/cell_std",1,0,0); add("/usb/cell_std/A.JPG",2,30,10);
    // Only MP4/JPG/JPEG enter a snapshot, including mixed-case extensions.
    add("/usb/DCIM/100MEDIA/OTHER.AVI",2,1000,10);
    add("/usb/DCIM/100MEDIA/OTHER.mov",2,1000,10);
    add("/usb/DCIM/100MEDIA/OTHER.PNG",2,1000,10);
    add("/usb/DCIM/100MEDIA/OTHER.JPG.tmp",2,1000,10);
    add("/usb/DCIM/100MEDIA/no_extension",2,1000,10);
    assert(collection_plan(available)); expect("files=3"); expect("first_baseline=1"); expect("cell_std_bytes=30");
    assert(collection_plan(available)); expect("new=0 changed=0 unchanged=3 missing=0");

    // A new file, reused name with changed size, and a removed old file.
    nodes[2].info.st_size=101;
    strcpy(nodes[3].path,"/usb/DCIM/100MEDIA/C.MP4");
    assert(collection_plan(available)); expect("new=1 changed=1 unchanged=1 missing=1");

    // Incomplete scans must not poison baseline (directory I/O and unavailable source).
    media_file_t *saved=baseline;
    directory_error=true;
    assert(!collection_plan(available)); assert(baseline==saved);
    directory_error=false; ready=false;
    assert(!collection_plan(available)); assert(baseline==saved);
    ready=true;
    assert(collection_plan(available)); expect("new=0 changed=0 unchanged=3 missing=0");
    nodes[2].info.st_mtime++;
    assert(collection_plan(available)); expect("new=0 changed=1 unchanged=2 missing=0");

    // Overflow is explicit and transactional; restoring the source retains old comparison.
    unsigned old_count=node_count;
    for(unsigned i=0;i<129;i++) {
        char path[256]; snprintf(path,sizeof(path),"/usb/DCIM/100MEDIA/EXTRA%u.JPG",i);
        add(path,2,i,0);
    }
    saved=baseline;
    assert(!collection_plan(available)); assert(baseline==saved);
    node_count=old_count;
    assert(collection_plan(available)); expect("unchanged=3");

    // Empty media trees are a valid baseline, unlike unreadable trees.
    node_count=0;
    assert(collection_plan(available)); expect("files=0"); expect("missing=3");
    assert(collection_plan(available)); expect("missing=0"); expect("first_baseline=0");

    collection_reset(); assert(!have_baseline);
    assert(collection_plan(available)); expect("first_baseline=1");
    collection_reset();
    puts("PASS: production planner metadata diffs, partial-scan rollback, overflow, schedule deadlines, bounded retries and OFF");
    return 0;
}
