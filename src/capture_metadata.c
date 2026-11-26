#include "capture_metadata.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    capture_read_fn read;
    void *context;
    uint64_t size;
    unsigned reads;
} reader_t;
static bool get(reader_t *r,uint64_t offset,void *data,size_t length) {
    if(offset>r->size || length>r->size-offset || ++r->reads>4096) return false;
    return r->read(r->context,offset,data,length);
}
static uint16_t u16(const uint8_t *p,bool le) {
    return le ? ((uint16_t)p[1]<<8)|p[0] : ((uint16_t)p[0]<<8)|p[1];
}
static uint32_t u32(const uint8_t *p,bool le) {
    return le ? ((uint32_t)u16(p+2,true)<<16)|u16(p,true)
              : ((uint32_t)u16(p,false)<<16)|u16(p+2,false);
}
static bool leap(unsigned year) { return year%4==0 && (year%100!=0 || year%400==0); }
static unsigned month_days(unsigned year,unsigned month) {
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1]+(month==2 && leap(year));
}
static bool date(capture_metadata_t *out,unsigned year,unsigned month,unsigned day,
                 unsigned hour,unsigned minute,unsigned second,const char *source) {
    if(!year || year>9999 || !month || month>12 || !day || day>31 || day>month_days(year,month) ||
       hour>23 || minute>59 || second>59) return false;
    snprintf(out->datetime,sizeof(out->datetime),"%04u-%02u-%02u %02u:%02u:%02u",
             year,month,day,hour,minute,second);
    out->source=source;
    return true;
}
static bool exif_date(const uint8_t *s,capture_metadata_t *out,const char *source) {
    for(unsigned i=0;i<19;i++) {
        char separator=i==4||i==7||i==13||i==16 ? ':' : i==10 ? ' ' : 0;
        if(separator ? s[i]!=separator : (s[i]<'0'||s[i]>'9')) return false;
    }
    unsigned y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
    return date(out,y,(s[5]-'0')*10+s[6]-'0',(s[8]-'0')*10+s[9]-'0',
                (s[11]-'0')*10+s[12]-'0',(s[14]-'0')*10+s[15]-'0',
                (s[17]-'0')*10+s[18]-'0',source);
}
bool capture_datetime_valid(const char *text) {
    if(!text || strlen(text)!=19 || text[4]!='-' || text[7]!='-') return false;
    uint8_t exif[19]; memcpy(exif,text,19); exif[4]=exif[7]=':';
    capture_metadata_t unused; return exif_date(exif,&unused,"validation");
}
static bool tiff_get(reader_t *r,uint64_t base,uint64_t end,uint32_t offset,void *data,size_t n) {
    return offset<=end-base && n<=end-base-offset && get(r,base+offset,data,n);
}
static bool exif_ifd(reader_t *r,uint64_t base,uint64_t end,uint32_t offset,bool le,
                     capture_metadata_t *out,uint32_t *exif_offset) {
    uint8_t count_bytes[2];
    if(!tiff_get(r,base,end,offset,count_bytes,2)) return false;
    unsigned count=u16(count_bytes,le);
    if(count>256 || offset>UINT32_MAX-2 || (uint64_t)offset+2+(uint64_t)count*12>end-base) return false;
    bool found=false;
    for(unsigned i=0;i<count;i++) {
        uint8_t entry[12];
        if(!tiff_get(r,base,end,offset+2+i*12,entry,12)) return false;
        unsigned tag=u16(entry,le),type=u16(entry+2,le);
        uint32_t length=u32(entry+4,le),value=u32(entry+8,le);
        if(tag==0x8769 && type==4 && length==1 && exif_offset) *exif_offset=value;
        if((tag==0x9003 || tag==0x9004) && type==2 && length>=19 && length<=256) {
            uint8_t text[19]; capture_metadata_t candidate;
            if(tiff_get(r,base,end,value,text,sizeof(text)) &&
               exif_date(text,&candidate,tag==0x9003?"exif_original":"exif_digitized")) {
                if(tag==0x9003) { *out=candidate; return true; }
                *out=candidate; found=true;
            }
        }
    }
    return found;
}
static bool jpeg_date(reader_t *r,capture_metadata_t *out) {
    uint8_t header[8];
    if(!get(r,0,header,2) || header[0]!=0xff || header[1]!=0xd8) return false;
    uint64_t pos=2;
    for(unsigned segment=0;segment<1024 && pos<r->size;segment++) {
        if(!get(r,pos,header,2) || header[0]!=0xff) return false;
        if(header[1]==0xff) { pos++; continue; }
        unsigned marker=header[1]; pos+=2;
        if(marker==0xda || marker==0xd9) return false; // Never read compressed image data.
        if(marker==0x01 || (marker>=0xd0 && marker<=0xd7)) continue;
        if(!get(r,pos,header,2)) return false;
        unsigned length=u16(header,false);
        if(length<2 || length>r->size-pos) return false;
        if(marker==0xe1 && length>=16 && get(r,pos+2,header,6) && !memcmp(header,"Exif\0\0",6)) {
            uint64_t base=pos+8,end=pos+length;
            if(!get(r,base,header,8)) return false;
            bool le=header[0]=='I' && header[1]=='I';
            if((le || (header[0]=='M' && header[1]=='M')) && u16(header+2,le)==42) {
                uint32_t exif=0;
                bool found=exif_ifd(r,base,end,u32(header+4,le),le,out,&exif);
                if(exif && exif_ifd(r,base,end,exif,le,out,NULL)) return true;
                if(found) return true;
            }
        }
        pos+=length;
    }
    return false;
}
static bool quicktime_date(uint64_t seconds,capture_metadata_t *out,const char *source) {
    if(!seconds) return false; // Unset creation time, not a real 1904 capture.
    uint64_t days=seconds/86400;
    if(days>366ULL*10000) return false;
    unsigned year=1904,month=1;
    while(year<=9999) {
        unsigned n=365+leap(year);
        if(days<n) break;
        days-=n; year++;
    }
    if(year>9999) return false;
    while(month<=12 && days>=month_days(year,month)) { days-=month_days(year,month); month++; }
    unsigned clock=seconds%86400;
    return date(out,year,month,days+1,clock/3600,(clock/60)%60,clock%60,source);
}
static bool mp4_boxes(reader_t *r,uint64_t start,uint64_t end,unsigned depth,capture_metadata_t *out) {
    if(depth>4) return false;
    bool found=false;
    for(uint64_t pos=start;pos<end;) {
        uint8_t box[16];
        if(end-pos<8 || !get(r,pos,box,8)) return found;
        uint64_t size=u32(box,false),header=8;
        if(size==1) {
            if(end-pos<16 || !get(r,pos+8,box+8,8)) return found;
            size=((uint64_t)u32(box+8,false)<<32)|u32(box+12,false); header=16;
        } else if(!size) size=end-pos;
        if(size<header || size>end-pos) return found;
        if(!memcmp(box+4,"mvhd",4) || !memcmp(box+4,"tkhd",4) || !memcmp(box+4,"mdhd",4)) {
            bool movie=!memcmp(box+4,"mvhd",4);
            uint8_t data[12]; capture_metadata_t candidate;
            if(size-header>=8 && get(r,pos+header,data,8)) {
                unsigned version=data[0]; uint64_t seconds=u32(data+4,false);
                if(version==1) {
                    if(size-header<12 || !get(r,pos+header+8,data+8,4)) return found;
                    seconds=(seconds<<32)|u32(data+8,false);
                }
                if(version<=1 && quicktime_date(seconds,&candidate,movie?"mp4_movie":"mp4_track")) {
                    if(movie) { *out=candidate; return true; }
                    if(!found) { *out=candidate; found=true; }
                }
            }
        } else if(!memcmp(box+4,"moov",4) || !memcmp(box+4,"trak",4) || !memcmp(box+4,"mdia",4)) {
            capture_metadata_t candidate;
            if(mp4_boxes(r,pos+header,pos+size,depth+1,&candidate)) {
                if(!strcmp(candidate.source,"mp4_movie")) { *out=candidate; return true; }
                if(!found) { *out=candidate; found=true; }
            }
        }
        pos+=size;
    }
    return found;
}
bool capture_metadata_read(capture_read_fn read,void *context,uint64_t size,bool jpeg,capture_metadata_t *out) {
    memset(out,0,sizeof(*out)); out->source="missing";
    if(!read) return false;
    reader_t reader={read,context,size,0};
    bool found=jpeg?jpeg_date(&reader,out):mp4_boxes(&reader,0,size,0,out);
    if(!found) { out->datetime[0]=0; out->source="missing"; }
    return found;
}
void capture_filename_time(const capture_metadata_t *metadata,char out[24]) {
    if(!metadata->datetime[0]) { strcpy(out,"UNKNOWN_CAPTURE_TIME"); return; }
    strcpy(out,metadata->datetime);
    out[10]='_'; out[13]='-'; out[16]='-';
}
