#pragma once
void test_ble_log(bool warning, const char *format, ...);
#define ESP_LOGI(tag, ...) test_ble_log(false, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_ble_log(true, __VA_ARGS__)
