#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../src/media_io.c"
static int64_t clock_us;
static unsigned delays;
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ticks) { assert(ticks==1); delays++; clock_us+=10000; }
static uint32_t old_crc(uint32_t raw,const uint8_t *data,size_t n) {
    for(size_t i=0;i<n;i++) {
        raw^=data[i];
        for(unsigned bit=0;bit<8;bit++) raw=(raw>>1)^((raw&1)?0xedb88320u:0);
    }
    return raw;
}
int main(void) {
    const uint8_t *vector=(const uint8_t *)"123456789";
    assert((media_crc32_update(UINT32_MAX,vector,9)^UINT32_MAX)==0xcbf43926u);
    uint32_t split=media_crc32_update(UINT32_MAX,vector,3);
    split=media_crc32_update(split,vector+3,2); split=media_crc32_update(split,vector+5,4);
    assert((split^UINT32_MAX)==0xcbf43926u);
    assert(media_crc32_update(UINT32_MAX,vector,0)==UINT32_MAX);
    uint8_t data[8192]; uint32_t random=1234;
    for(unsigned i=0;i<sizeof(data);i++) { random=random*1664525+1013904223; data[i]=random>>24; }
    for(size_t n=0;n<=sizeof(data);n+=127) assert(media_crc32_update(UINT32_MAX,data,n)==old_crc(UINT32_MAX,data,n));
    assert(media_crc32_update(UINT32_MAX,data,sizeof(data))==old_crc(UINT32_MAX,data,sizeof(data)));
    media_io_guard_t guard; media_io_begin(&guard);
    for(unsigned i=0;i<1000;i++) { clock_us=99*i; media_io_checkpoint(&guard); }
    assert(!delays && !guard.yields); // No unconditional delay per buffer.
    clock_us=100000; media_io_checkpoint(&guard); assert(delays==1 && guard.yields==1 && guard.last_yield_us==110000);
    clock_us=209999; media_io_checkpoint(&guard); assert(delays==1);
    clock_us=210000; media_io_checkpoint(&guard); assert(delays==2);
    puts("PASS ROM CRC compatibility (Espressif host reference) and bounded periodic I/O yielding");
    return 0;
}
