// Production parsers and naming, exercised with known metadata and damaged files.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/capture_metadata.c"
#include "../src/archive_naming.c"

static uint8_t fixture[512];
static unsigned reads;
static bool read_fixture(void *context,uint64_t offset,void *out,size_t length) {
    size_t size=*(size_t *)context;
    assert(offset<=size && length<=size-offset); assert(++reads<=4096);
    if(!memcmp(fixture+4,"ftyp",4)) {
        unsigned payload=fixture[27]==1?40:32;
        assert(offset>=48 || offset+length<=payload); // Skip compressed video data.
    }
    memcpy(out,fixture+(size_t)offset,length); return true;
}
static void put16(uint8_t *p,unsigned value,bool le) {
    p[le?0:1]=value; p[le?1:0]=value>>8;
}
static void put32(uint8_t *p,uint32_t value,bool le) {
    for(unsigned i=0;i<4;i++) p[le?i:3-i]=value>>(i*8);
}
static void entry(uint8_t *p,unsigned tag,unsigned type,uint32_t count,uint32_t value,bool le) {
    put16(p,tag,le); put16(p+2,type,le); put32(p+4,count,le); put32(p+8,value,le);
}
static size_t jpeg_fixture(bool le) {
    memset(fixture,0,sizeof(fixture)); reads=0;
    fixture[0]=0xff; fixture[1]=0xd8; fixture[2]=0xff; fixture[3]=0xe1;
    put16(fixture+4,104,false); memcpy(fixture+6,"Exif\0\0",6);
    uint8_t *t=fixture+12;
    t[0]=t[1]=le?'I':'M'; put16(t+2,42,le); put32(t+4,8,le);
    put16(t+8,1,le); entry(t+10,0x8769,4,1,26,le);
    put16(t+26,2,le);
    // Digitized comes first; DateTimeOriginal must take precedence.
    entry(t+28,0x9004,2,20,56,le); entry(t+40,0x9003,2,20,76,le);
    memcpy(t+56,"2026:11:26 14:31:05",20); memcpy(t+76,"2026:11:26 14:30:05",20);
    fixture[108]=0xff; fixture[109]=0xd9;
    return 110;
}
static size_t mp4_fixture(bool v1) {
    memset(fixture,0,sizeof(fixture)); reads=0;
    put32(fixture,24,false); memcpy(fixture+4,"ftyp",4);
    put32(fixture+24,24,false); memcpy(fixture+28,"mdat",4);
    memset(fixture+32,0xab,16);
    put32(fixture+48,v1?36:32,false); memcpy(fixture+52,"moov",4);
    put32(fixture+56,v1?28:24,false); memcpy(fixture+60,"mvhd",4);
    fixture[64]=v1?1:0;
    if(v1) { put32(fixture+68,1,false); put32(fixture+72,340963309,false); }
    else put32(fixture+68,UINT32_C(3878548205),false);
    return v1?84:80;
}
int main(void) {
    capture_metadata_t metadata;
    for(unsigned le=0;le<2;le++) {
        size_t size=jpeg_fixture(le);
        assert(capture_metadata_read(read_fixture,&size,size,true,&metadata));
        assert(!strcmp(metadata.datetime,"2026-11-26 14:30:05"));
        assert(!strcmp(metadata.source,"exif_original"));
        char stamp[24]; capture_filename_time(&metadata,stamp);
        assert(!strcmp(stamp,"2026-11-26_14-30-05"));
        // Invalid calendar dates and external TIFF offsets cannot be trusted.
        memcpy(fixture+12+76,"2025:02:29 14:30:05",20);
        assert(capture_metadata_read(read_fixture,&size,size,true,&metadata));
        assert(!strcmp(metadata.source,"exif_digitized"));
        put32(fixture+12+28+8,UINT32_MAX,le);
        assert(!capture_metadata_read(read_fixture,&size,size,true,&metadata));
        assert(!metadata.datetime[0] && !strcmp(metadata.source,"missing"));
        size=jpeg_fixture(le); put16(fixture+12+26,65535,le);
        assert(!capture_metadata_read(read_fixture,&size,size,true,&metadata));
        size=jpeg_fixture(le);
        for(size_t truncated=0;truncated<size;truncated++) {
            reads=0;
            capture_metadata_read(read_fixture,&truncated,truncated,true,&metadata);
        }
    }
    size_t size=mp4_fixture(false);
    assert(capture_metadata_read(read_fixture,&size,size,false,&metadata));
    assert(!strcmp(metadata.datetime,"2026-11-26 14:30:05"));
    assert(!strcmp(metadata.source,"mp4_movie"));
    size=mp4_fixture(true);
    assert(capture_metadata_read(read_fixture,&size,size,false,&metadata));
    assert(!strcmp(metadata.datetime,"2050-11-26 14:30:05")); // 64-bit v1 beyond 2038.
    // Movie time zero can fall back to an actual track header.
    size=mp4_fixture(false); put32(fixture+68,0,false); memcpy(fixture+60,"tkhd",4);
    assert(!capture_metadata_read(read_fixture,&size,size,false,&metadata));
    put32(fixture+68,UINT32_C(3878548205),false);
    assert(capture_metadata_read(read_fixture,&size,size,false,&metadata));
    assert(!strcmp(metadata.source,"mp4_track"));
    // Extended-size mdat is skipped without inspecting its image/video payload.
    size=mp4_fixture(false); put32(fixture+24,1,false); put32(fixture+32,0,false); put32(fixture+36,24,false);
    assert(capture_metadata_read(read_fixture,&size,size,false,&metadata));
    for(size_t truncated=0;truncated<size;truncated++) {
        reads=0;
        capture_metadata_read(read_fixture,&truncated,truncated,false,&metadata);
    }
    size=mp4_fixture(false); put32(fixture+24,UINT32_MAX,false);
    assert(!capture_metadata_read(read_fixture,&size,size,false,&metadata));
    size=mp4_fixture(true); memset(fixture+68,0xff,8);
    assert(!capture_metadata_read(read_fixture,&size,size,false,&metadata));

    uint8_t mac[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff}; char prefix[17];
    archive_device_prefix(mac,prefix); assert(!strcmp(prefix,"ESP-AABBCCDDEEFF"));
    size=mp4_fixture(false); assert(capture_metadata_read(read_fixture,&size,size,false,&metadata));
    char sha[65],capture_id[65],path[768],other[768],key[256],second_key[256];
    memset(sha,'a',64); sha[64]=0; memset(capture_id,'c',64); capture_id[64]=0;
    assert(archive_media_path(path,sizeof(path),"/sd/stonelink",prefix,"/camera/a.MP4",&metadata,sha,capture_id,"SD"));
    assert(strstr(path,"ESP-AABBCCDDEEFF_2026-11-26_14-30-05_VIDEO_SD_"));
    archive_asset_t parsed;
    assert(archive_parse_filename(strrchr(path,'/')+1,prefix,&parsed));
    assert(!strcmp(parsed.kind,"VIDEO") && !strcmp(parsed.quality,"SD") && !strcmp(parsed.sha256,sha));
    assert(!strcmp(parsed.capture.datetime,metadata.datetime));
    assert(archive_quality_path(other,sizeof(other),path,prefix,"HD"));
    assert(strstr(other,"_VIDEO_HD_") && strcmp(path,other));
    assert(archive_capture_key(key,sizeof(key),prefix,"/usb/DCIM/101MEDIA/DSCF0001.MP4",&metadata));
    assert(archive_capture_key(second_key,sizeof(second_key),prefix,"/different-camera/another-name.mp4",&metadata));
    assert(!strcmp(key,second_key)); // Folder and original name never determine matching.
    assert(archive_capture_key(second_key,sizeof(second_key),prefix,"/camera/photo.jpg",&metadata));
    assert(strcmp(key,second_key)); // A photo and video at the same second are separate.
    assert(archive_capture_key(key,sizeof(key),prefix,"/camera/photo.jpeg",&metadata));
    assert(!strcmp(key,second_key)); // JPG and JPEG are the same PHOTO family.
    capture_metadata_t later=metadata; later.datetime[18]='6';
    assert(archive_capture_key(second_key,sizeof(second_key),prefix,"/camera/photo.jpg",&later));
    assert(strcmp(key,second_key));
    sha[0]='b';
    assert(archive_media_path(other,sizeof(other),"/sd/stonelink",prefix,"/camera/a.MP4",&metadata,sha,capture_id,"SD"));
    assert(strcmp(path,other));
    mac[5]=0xfe; archive_device_prefix(mac,prefix);
    assert(archive_media_path(other,sizeof(other),"/sd/stonelink",prefix,"/camera/a.MP4",&metadata,sha,capture_id,"SD"));
    assert(strstr(other,"ESP-AABBCCDDEEFE"));
    assert(!archive_media_path(other,10,"/sd/stonelink",prefix,"/usb/DCIM/x.jpg",&metadata,sha,capture_id,"SD"));
    assert(!archive_media_path(other,sizeof(other),"/sd/stonelink",prefix,"/usb/DCIM/x.dat",&metadata,sha,capture_id,"SD"));
    capture_metadata_t missing={"","missing"};
    assert(!archive_capture_key(key,sizeof(key),prefix,"a.jpg",&missing));
    assert(archive_media_path(other,sizeof(other),"/sd/stonelink",prefix,"/usb/DCIM/x.JPEG",&missing,sha,capture_id,"SD"));
    assert(strstr(other,"UNKNOWN_CAPTURE_TIME_PHOTO_SD_"));
    assert(archive_parse_filename(strrchr(other,'/')+1,prefix,&parsed) && !parsed.capture.datetime[0]);
    archive_rank_t rank={0};
    archive_rank_add(&rank,100); assert(!strcmp(archive_rank_quality(&rank,100,true),"SD"));
    archive_rank_add(&rank,200); assert(!strcmp(archive_rank_quality(&rank,200,true),"HD"));
    assert(!strcmp(archive_rank_quality(&rank,100,true),"SD"));
    archive_rank_add(&rank,200); assert(!strcmp(archive_rank_quality(&rank,200,true),"SD"));
    archive_rank_add(&rank,300); assert(!strcmp(archive_rank_quality(&rank,300,true),"HD"));
    assert(!strcmp(archive_rank_quality(&rank,300,false),"SD"));
    // Deterministic malformed bytes: callbacks assert every read stays in bounds.
    uint32_t random=12345;
    for(unsigned trial=0;trial<1000;trial++) {
        for(unsigned i=0;i<sizeof(fixture);i++) {
            random=random*1664525u+1013904223u; fixture[i]=random>>24;
        }
        if(trial&1) { fixture[0]=0xff; fixture[1]=0xd8; }
        size=trial%sizeof(fixture); reads=0;
        capture_metadata_read(read_fixture,&size,size,trial&1,&metadata);
    }
    puts("PASS capture metadata/naming: EXIF endian/date/bounds, MP4 v0/v1, unknown dates, prefix/type/size flags, filename roundtrip");
    return 0;
}
