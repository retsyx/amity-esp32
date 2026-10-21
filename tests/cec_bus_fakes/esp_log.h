#pragma once
void test_cec_log(const char *tag, const char *format, ...);
#define ESP_LOGW(tag, ...) test_cec_log(tag, __VA_ARGS__)
