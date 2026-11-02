#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace system_sync_test_log {
inline std::vector<std::string> messages;
template<class... Args>void capture(const char*,const char* format,Args... args){
    char text[256];std::snprintf(text,sizeof(text),format,args...);messages.emplace_back(text);
}
}
#define ESP_LOGW system_sync_test_log::capture
