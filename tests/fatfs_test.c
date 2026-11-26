// Production FatFs with a sparse RAM disk: never opens physical Windows disks.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "ff.h"
#include "diskio.h"

#define SLOTS 32768
static struct { DWORD key; unsigned used; BYTE data[512]; } sectors[SLOTS];
static DWORD sector_count;
const PARTITION VolToPart[FF_VOLUMES]={{0,0},{1,0}};
static BYTE *lookup(DWORD sector, int create)
{
    unsigned i=sector%SLOTS;
    for(unsigned tries=0;tries<SLOTS;tries++,i=(i+1)%SLOTS) {
        if(sectors[i].used && sectors[i].key==sector) return sectors[i].data;
        if(!sectors[i].used) {
            if(!create) return NULL;
            sectors[i].used=1; sectors[i].key=sector; return sectors[i].data;
        }
    }
    assert(!"sparse disk capacity exceeded"); return NULL;
}
DSTATUS disk_initialize(BYTE drive) { return 0; }
DSTATUS disk_status(BYTE drive) { return 0; }
DRESULT disk_read(BYTE drive,BYTE *data,LBA_t sector,UINT count)
{
    if(sector>=sector_count || count>sector_count-sector) return RES_PARERR;
    for(UINT i=0;i<count;i++) {
        BYTE *saved=lookup(sector+i,0);
        if(saved) memcpy(data+i*512,saved,512); else memset(data+i*512,0,512);
    }
    return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *data,LBA_t sector,UINT count)
{
    if(sector>=sector_count || count>sector_count-sector) return RES_PARERR;
    for(UINT i=0;i<count;i++) memcpy(lookup(sector+i,1),data+i*512,512);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer)
{
    switch(command) {
    case CTRL_SYNC: case CTRL_TRIM: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buffer=sector_count; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer=512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer=1; return RES_OK;
    default: return RES_PARERR;
    }
}
void *ff_memalloc(UINT size) { return malloc(size); }
void ff_memfree(void *ptr) { free(ptr); }
int ff_mutex_create(int vol) { return 1; }
void ff_mutex_delete(int vol) { }
int ff_mutex_take(int vol) { return 1; }
void ff_mutex_give(int vol) { }
DWORD get_fattime(void) { return (46u<<25)|(11u<<21)|(25u<<16); }

static void roundtrip(BYTE format,DWORD count,BYTE expected_type,DWORD cluster)
{
    memset(sectors,0,sizeof(sectors)); sector_count=count;
    MKFS_PARM options={0}; options.fmt=format|FM_SFD; options.au_size=cluster;
    BYTE work[4096]; FATFS fs; FIL file; UINT n; char result[64];
    assert(f_mkfs("0:",&options,work,sizeof(work))==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK); assert(fs.fs_type==expected_type);
    assert(f_mkdir("0:/archive")==FR_OK);
    const char *path="0:/archive/a long camera media filename.mp4";
    const char *payload="StoneLink FAT32 and exFAT coexist";
    assert(f_open(&file,path,FA_WRITE|FA_CREATE_NEW)==FR_OK);
    assert(f_write(&file,payload,strlen(payload),&n)==FR_OK && n==strlen(payload));
    assert(f_sync(&file)==FR_OK); assert(f_close(&file)==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK);
    assert(f_open(&file,path,FA_READ)==FR_OK);
    assert(f_read(&file,result,sizeof(result),&n)==FR_OK && n==strlen(payload));
    assert(!memcmp(result,payload,n)); assert(f_close(&file)==FR_OK);
    DWORD free_clusters; FATFS *mounted;
    assert(f_getfree("0:",&free_clusters,&mounted)==FR_OK);
    assert((unsigned long long)(mounted->n_fatent-2)*mounted->csize*512 > (expected_type==FS_EXFAT ? 32ULL*1024*1024*1024 : 0));
    assert(f_mount(NULL,"0:",0)==FR_OK);
}
int main(void)
{
    assert(FF_FS_EXFAT==1 && FF_USE_LFN!=0);
    assert(sizeof(FSIZE_t)==8);
    roundtrip(FM_FAT32,131072,FS_FAT32,512); // 64 MiB FAT32
    roundtrip(FM_EXFAT,134217728,FS_EXFAT,131072); // 64 GiB exFAT
    puts("PASS: same production FatFs formats/mounts FAT32 and 64 GiB exFAT; long-name write/sync/remount/read and capacity checks");
    return 0;
}
