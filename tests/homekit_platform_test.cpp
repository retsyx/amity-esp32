#include "config_store.hpp"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "hap_platform_keystore.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
thread_local bool internal_stack = false;
bool dispatch_enabled = true;
unsigned dispatches = 0;
std::vector<std::string> calls;
std::string failure;
std::vector<std::string> warnings;
constexpr int handle_id = 17;
const esp_partition_t key_partition{1};
httpd_config_t server_config{};

int operation(const char *name) {
    assert(internal_stack);
    calls.emplace_back(name);
    return failure == name ? ESP_FAIL : ESP_OK;
}
void check_partition(const char *partition) { assert(std::strcmp(partition, "nvs") == 0); }
void check_handle(nvs_handle handle) { assert(handle == handle_id); }
void clear(const char *fail = "") { calls.clear(); failure = fail; }
}

namespace config_store {
bool with_internal_flash_stack(std::function<void()> callback) {
    ++dispatches;
    assert(!internal_stack);
    if (!dispatch_enabled) return false;
    std::thread worker([&] { internal_stack = true; callback(); });
    worker.join();
    return true;
}
}

extern "C" {
int hap_platform_httpd_start(httpd_handle_t *handle);
int hap_platform_httpd_stop(httpd_handle_t *handle);
httpd_handle_t *hap_platform_httpd_get_handle();

void amity_fake_log_warning(const char *tag, const char *format, ...) {
    assert(!std::strcmp(tag, "hap_httpd"));
    char message[128];
    va_list args; va_start(args, format);
    const int size = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    assert(size > 0 && size < static_cast<int>(sizeof(message)));
    warnings.emplace_back(message);
}

esp_err_t nvs_flash_init_partition(const char *partition) {
    check_partition(partition); return operation("init");
}
esp_partition_iterator_t esp_partition_find(int type, int subtype, const char *label) {
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS && !label);
    operation("find_keys"); return &key_partition;
}
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator) {
    assert(iterator == &key_partition); operation("get_keys"); return iterator;
}
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *partition, nvs_sec_cfg_t *config) {
    assert(partition == &key_partition); std::memset(config, 0x5a, sizeof(*config)); return operation("read_keys");
}
esp_err_t nvs_flash_secure_init_partition(const char *partition, nvs_sec_cfg_t *config) {
    check_partition(partition);
    assert(config && std::all_of(std::begin(config->key), std::end(config->key), [](auto byte) { return byte == 0x5a; }));
    return operation("secure_init");
}
esp_err_t nvs_open_from_partition(const char *partition, const char *space, int mode, nvs_handle *handle) {
    check_partition(partition); assert(std::strcmp(space, "hap_ctrl") == 0);
    assert(mode == NVS_READONLY || mode == NVS_READWRITE); *handle = handle_id; return operation("open");
}
esp_err_t nvs_get_blob(nvs_handle handle, const char *key, void *value, size_t *size) {
    check_handle(handle); assert(std::strcmp(key, "controller") == 0 && *size == 4);
    std::memcpy(value, "test", 4); *size = 4; return operation("get");
}
esp_err_t nvs_set_blob(nvs_handle handle, const char *key, const void *value, size_t size) {
    check_handle(handle); assert(std::strcmp(key, "controller") == 0 && size == 4 && std::memcmp(value, "test", 4) == 0);
    return operation("set");
}
esp_err_t nvs_commit(nvs_handle handle) { check_handle(handle); return operation("commit"); }
esp_err_t nvs_erase_key(nvs_handle handle, const char *key) {
    check_handle(handle); assert(std::strcmp(key, "controller") == 0); return operation("erase_key");
}
esp_err_t nvs_erase_all(nvs_handle handle) { check_handle(handle); return operation("erase_all"); }
void nvs_close(nvs_handle handle) { check_handle(handle); operation("close"); }

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
    server_config = *config;
    // Reproduce the soak's fragmented internal heap: enough total RAM remains,
    // but its largest block cannot hold the full 12 KiB HTTP stack.
    if ((config->task_caps & MALLOC_CAP_INTERNAL) && config->stack_size > 11264) return ESP_FAIL;
    *handle = reinterpret_cast<void *>(1); return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t handle) { assert(handle == reinterpret_cast<void *>(1)); return ESP_OK; }
}

int main() {
    assert(std::strcmp(hap_platform_keystore_get_nvs_partition_name(), "nvs") == 0);
    assert(std::strcmp(hap_platform_keystore_get_factory_nvs_partition_name(), "nvs") == 0);
    for (bool read_only : {false, true}) {
        clear(); assert(hap_platform_keystore_init_partition("nvs", read_only) == 0);
#ifdef CONFIG_NVS_ENCRYPTION
        assert((calls == std::vector<std::string>{"find_keys", "get_keys", "read_keys", "secure_init"}));
        clear("secure_init");
#else
        assert((calls == std::vector<std::string>{"init"}));
        clear("init");
#endif
        assert(hap_platform_keystore_init_partition("nvs", read_only) == -1);
    }
    auto get = [] { uint8_t value[4]{}; size_t size = sizeof(value); return hap_platform_keystore_get("nvs", "hap_ctrl", "controller", value, &size); };
    auto set = [] { return hap_platform_keystore_set("nvs", "hap_ctrl", "controller", reinterpret_cast<const uint8_t *>("test"), 4); };
    auto erase = [] { return hap_platform_keystore_delete("nvs", "hap_ctrl", "controller"); };
    auto erase_namespace = [] { return hap_platform_keystore_delete_namespace("nvs", "hap_ctrl"); };
    struct Case { std::function<int()> run; std::vector<std::string> expected; };
    const std::vector<Case> cases{
        {get, {"open", "get", "close"}},
        {set, {"open", "set", "commit", "close"}},
        {erase, {"open", "erase_key", "commit", "close"}},
        {erase_namespace, {"open", "erase_all", "commit", "close"}},
    };
    for (const auto &test : cases) {
        clear(); assert(test.run() == 0 && calls == test.expected);
        for (const auto &step : test.expected) {
            if (step == "close") continue;
            clear(step.c_str()); assert(test.run() == -1);
            assert(calls.front() == "open");
            if (step == "open") assert(calls.size() == 1);
            else assert(calls.back() == "close");
            if (step != "commit") assert(std::find(calls.begin(), calls.end(), "commit") == calls.end());
        }
        clear(); dispatch_enabled = false; const auto before = dispatches;
        assert(test.run() == -1 && calls.empty() && dispatches == before + 1);
        dispatch_enabled = true;
    }
    for (bool read_only : {false, true}) {
        clear(); dispatch_enabled = false;
        assert(hap_platform_keystore_init_partition("nvs", read_only) == -1 && calls.empty());
        dispatch_enabled = true;
    }
    clear(); const auto before = dispatches;
    assert(hap_platfrom_keystore_erase_partition("nvs") == -1 && calls.empty() && dispatches == before);
    httpd_handle_t server = nullptr;
    assert(hap_platform_httpd_start(&server) == ESP_OK && server);
    assert(server_config.stack_size == 12288);
    assert(server_config.task_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(server_config.server_port == 51826 && server_config.ctrl_port == 32859 && server_config.max_open_sockets == 4);
    assert(server_config.open_fn && server_config.recv_wait_timeout == 5 && server_config.send_wait_timeout == 5);
    // The actual platform hook runs for every accepted connection, before
    // pair setup/verify. Use native TCP options without a network connection.
    auto option = [](int fd, int level, int name) {
        int value = -1; socklen_t length = sizeof(value);
        assert(!getsockopt(fd, level, name, &value, &length) && length == sizeof(value));
        return value;
    };
    const int first = socket(AF_INET, SOCK_STREAM, 0), second = socket(AF_INET, SOCK_STREAM, 0);
    assert(first >= 0 && second >= 0);
    const int keep_alive = 1;
    assert(!setsockopt(first, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(keep_alive)));
    assert(option(first, IPPROTO_TCP, TCP_NODELAY) == 0 && option(second, IPPROTO_TCP, TCP_NODELAY) == 0);
    assert(server_config.open_fn(server, first) == ESP_OK && warnings.empty());
    assert(option(first, IPPROTO_TCP, TCP_NODELAY) != 0 && option(second, IPPROTO_TCP, TCP_NODELAY) == 0);
    assert(option(first, SOL_SOCKET, SO_KEEPALIVE) != 0);
    assert(server_config.open_fn(server, second) == ESP_OK && warnings.empty());
    assert(option(second, IPPROTO_TCP, TCP_NODELAY) != 0 && option(second, SOL_SOCKET, SO_KEEPALIVE) == 0);
    assert(server_config.open_fn(server, first) == ESP_OK && warnings.empty());
    // A socket-option failure is visible without replacing the server or
    // changing the other connections' socket options.
    assert(server_config.open_fn(server, -1) == ESP_OK && warnings.size() == 1);
    assert(warnings.front().find("TCP_NODELAY") != std::string::npos);
    assert(warnings.front().find(std::to_string(EBADF)) != std::string::npos);
    assert(option(first, IPPROTO_TCP, TCP_NODELAY) != 0 && option(second, IPPROTO_TCP, TCP_NODELAY) != 0);
    assert(!close(first) && !close(second));
    assert(hap_platform_httpd_get_handle() == &server);
    assert(hap_platform_httpd_stop(&server) == ESP_OK && !hap_platform_httpd_get_handle());
    std::cout << "HomeKit platform: native TCP_NODELAY per accepted socket, option failure warning and preserved keepalive; PSRAM HTTP stack and internal-stack keystore passed\n";
}
