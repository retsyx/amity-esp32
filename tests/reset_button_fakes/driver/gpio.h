#pragma once
#include "esp_err.h"
#include <cstdint>
using gpio_num_t = int;
using gpio_isr_t = void (*)(void *);
constexpr int GPIO_MODE_INPUT = 1, GPIO_PULLUP_ENABLE = 1, GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0, GPIO_INTR_ANYEDGE = 3;
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49)
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
};
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(gpio_num_t);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_set_intr_type(gpio_num_t, int);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_uninstall_isr_service();
esp_err_t gpio_isr_handler_add(gpio_num_t, gpio_isr_t, void *);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
