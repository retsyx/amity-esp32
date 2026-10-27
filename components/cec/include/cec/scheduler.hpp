/* Copyright 2026. GPL-3.0-or-later. */
#pragma once
#include "cec/engine.hpp"
#include "amity/hardware_limits.hpp"

namespace cec {
// Software capacity, not a hardware-qualified port-count claim. The current
// board registry exposes two ports. Callers serialize attachment and ticking.
class EngineBank {
public:
    static constexpr size_t capacity = amity::max_cec_ports;
    int attach(Engine &engine, uint8_t gpio);
    void detach(Engine &engine);
    // All inputs have already been sampled. No output is applied until the
    // complete bank returns, including ports wired to the same external bus.
    uint64_t CEC_IRAM tick(uint64_t now, uint64_t input_levels);
private:
    Engine *engines_[capacity]{};
    uint8_t gpios_[capacity]{};
    uint64_t pins_[capacity]{};
};
}
