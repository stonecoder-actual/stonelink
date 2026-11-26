#include "media_io.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
uint32_t media_crc32_update(uint32_t raw,const uint8_t *data,size_t length) {
    while(length) {
        uint32_t n=length>UINT32_MAX?UINT32_MAX:(uint32_t)length;
        raw=~esp_rom_crc32_le(~raw,data,n); data+=n; length-=n;
    }
    return raw;
}
void media_io_begin(media_io_guard_t *guard) { guard->last_yield_us=esp_timer_get_time(); guard->yields=0; }
void media_io_checkpoint(media_io_guard_t *guard) {
    if(esp_timer_get_time()-guard->last_yield_us>=100000) {
        vTaskDelay(1); guard->last_yield_us=esp_timer_get_time(); guard->yields++;
    }
}
