// Actual reconciliation code with an in-memory local directory and rename faults.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "dirent.h"
#include "sys/stat.h"
#include "../src/capture_metadata.c"
#include "../src/archive_naming.c"

static struct { char path[768]; int64_t bytes; } nodes[20];
static unsigned count,moves,fail_move;
static const char *prefix="ESP-AABBCCDDEEFF";
void test_log(const char *format,...) { (void)format; }
int stat(const char *path,struct stat *out) {
    if(!strcmp(path,"/archive")) { *out=(struct stat){1,0,0}; return 0; }
    for(unsigned i=0;i<count;i++) if(!strcmp(path,nodes[i].path)) { *out=(struct stat){2,nodes[i].bytes,0}; return 0; }
    errno=ENOENT; return -1;
}
DIR *opendir(const char *path) {
    if(strcmp(path,"/archive")) { errno=ENOENT; return NULL; }
    DIR *dir=calloc(1,sizeof(*dir)); assert(dir); strcpy(dir->path,path); return dir;
}
struct dirent *readdir(DIR *dir) {
    if(dir->next==count) return NULL;
    strcpy(dir->entry.d_name,strrchr(nodes[dir->next++].path,'/')+1); return &dir->entry;
}
int closedir(DIR *dir) { free(dir); return 0; }
int rename(const char *from,const char *to) {
    if(++moves==fail_move) { errno=EIO; return -1; }
    struct stat info;
    if(!stat(to,&info)) { errno=EEXIST; return -1; }
    for(unsigned i=0;i<count;i++) if(!strcmp(from,nodes[i].path)) { strcpy(nodes[i].path,to); return 0; }
    errno=ENOENT; return -1;
}
#include "../src/media_group.c"
static void add_asset(char hash,uint64_t bytes,const char *quality,bool known,const char *kind) {
    assert(count<20); char sha[65]; memset(sha,hash,64); sha[64]=0;
    capture_metadata_t metadata={"2026-11-26 14:30:05","test"};
    if(!known) metadata.datetime[0]=0;
    char leaf[160]; assert(filename(leaf,sizeof(leaf),prefix,&metadata,kind,quality,sha));
    snprintf(nodes[count].path,sizeof(nodes[count].path),"/archive/%s",leaf); nodes[count++].bytes=bytes;
}
static void quality(unsigned index,const char *expected) {
    archive_asset_t asset; assert(archive_parse_filename(strrchr(nodes[index].path,'/')+1,prefix,&asset));
    assert(!strcmp(asset.quality,expected));
}
static void reset(void) { count=moves=fail_move=0; }
int main(void) {
    media_group_result_t result;
    reset(); add_asset('a',100,"SD",true,"PHOTO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD"); assert(!result.hd_path[0]);
    add_asset('b',200,"SD",true,"PHOTO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD"); quality(1,"HD");
    assert(result.rank.count==2 && strstr(result.hd_path,"_PHOTO_HD_"));
    add_asset('c',300,"SD",true,"PHOTO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(1,"SD"); quality(2,"HD");
    char sha[65],found[768]; memset(sha,'b',64); sha[64]=0;
    assert(media_group_find("/archive",prefix,sha,found,sizeof(found)) && !strcmp(found,nodes[1].path));
    add_asset('d',300,"SD",true,"PHOTO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result));
    for(unsigned i=0;i<count;i++) quality(i,"SD");
    assert(result.rank.max_count==2 && !result.hd_path[0]);
    add_asset('e',400,"SD",true,"PHOTO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(4,"HD");
    unsigned previous=moves;
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); assert(previous==moves); // Idempotent repeat.

    reset(); add_asset('a',200,"SD",true,"VIDEO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD");
    add_asset('b',100,"SD",true,"VIDEO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"HD"); quality(1,"SD");
    add_asset('c',50,"SD",true,"VIDEO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"HD"); quality(2,"SD");

    // Missing dates cannot associate distinct assets or create an HD claim.
    reset(); add_asset('a',100,"HD",false,"VIDEO"); add_asset('b',200,"SD",false,"VIDEO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD"); quality(1,"SD");
    reset(); add_asset('a',100,"HD",true,"VIDEO");
    assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD");

    // Abort before relabeling a mixed-type group; production also validates ID.
    reset(); add_asset('a',100,"SD",true,"VIDEO"); add_asset('b',200,"SD",true,"PHOTO");
    assert(!media_group_reconcile("/archive",prefix,NULL,NULL,&result)); assert(!moves);
    reset(); add_asset('a',100,"SD",true,"PHOTO"); add_asset('a',100,"HD",true,"PHOTO");
    assert(!media_group_reconcile("/archive",prefix,NULL,NULL,&result)); assert(!moves); // Duplicate aliases preserved.

    // Failed demotion/promotion is recoverable without payload deletion.
    for(unsigned fail=1;fail<=2;fail++) {
        reset(); add_asset('a',100,"HD",true,"VIDEO"); add_asset('b',200,"SD",true,"VIDEO"); fail_move=fail;
        assert(!media_group_reconcile("/archive",prefix,NULL,NULL,&result)); assert(count==2);
        if(fail==2) { quality(0,"SD"); quality(1,"SD"); } // Demote before promoting.
        fail_move=0;
        assert(media_group_reconcile("/archive",prefix,NULL,NULL,&result)); quality(0,"SD"); quality(1,"HD");
    }
    puts("PASS group filenames: lone SD, size-based HD, later arrivals, ties, type separation, rename-fault recovery");
    return 0;
}
