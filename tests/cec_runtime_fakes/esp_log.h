#pragma once
void fixture_log_warning(const char* tag, const char* format, ...);
#define ESP_LOGW(tag, ...) fixture_log_warning(tag, __VA_ARGS__)
