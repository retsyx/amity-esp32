/* Copyright 2026. GPL-3.0-or-later. */
#include "cec/scheduler.hpp"

namespace cec {
int EngineBank::attach(Engine &engine, uint8_t gpio) {
    if (gpio >= 64) return -1;
    int empty = -1;
    for (size_t i = 0; i < capacity; ++i) {
        if (engines_[i] == &engine || (engines_[i] && gpios_[i] == gpio)) return -1;
        if (!engines_[i] && empty < 0) empty = static_cast<int>(i);
    }
    if (empty >= 0) {
        engines_[empty] = &engine;
        gpios_[empty] = gpio;
        pins_[empty] = uint64_t{1} << gpio;
    }
    return empty;
}
void EngineBank::detach(Engine &engine) {
    for (auto &entry : engines_) if (entry == &engine) entry = nullptr;
}
uint64_t EngineBank::tick(uint64_t now, uint64_t input_levels) {
    uint64_t pull_low = 0;
    for (size_t i = 0; i < capacity; ++i) if (engines_[i]) {
        const uint64_t pin = pins_[i];
        if (engines_[i]->tick(now, (input_levels & pin) != 0)) pull_low |= pin;
    }
    return pull_low;
}
}
