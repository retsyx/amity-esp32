#pragma once
#include <functional>
namespace config_store {
bool with_internal_flash_stack(std::function<void()> operation);
}
