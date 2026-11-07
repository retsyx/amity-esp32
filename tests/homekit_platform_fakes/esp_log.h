#pragma once
static inline void amity_fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE(...) amity_fake_log(__VA_ARGS__)
#define ESP_LOGI(...) amity_fake_log(__VA_ARGS__)
#ifdef __cplusplus
extern "C" {
#endif
void amity_fake_log_warning(const char *tag, const char *format, ...);
#ifdef __cplusplus
}
#endif
#define ESP_LOGW(...) amity_fake_log_warning(__VA_ARGS__)
