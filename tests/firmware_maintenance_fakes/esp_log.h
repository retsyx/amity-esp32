#pragma once
#include <cstdio>
#define ESP_LOGW(tag,fmt,...) std::fprintf(stderr,"W %s: " fmt "\n",tag,##__VA_ARGS__)
#define ESP_LOGI(tag,fmt,...) std::fprintf(stderr,"I %s: " fmt "\n",tag,##__VA_ARGS__)
#define ESP_LOGE(tag,fmt,...) std::fprintf(stderr,"E %s: " fmt "\n",tag,##__VA_ARGS__)
