#pragma once
void test_log(const char *format, ...);
#define ESP_LOGI(tag, ...) test_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log(__VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log(__VA_ARGS__)
