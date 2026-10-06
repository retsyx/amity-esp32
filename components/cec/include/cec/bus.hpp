#pragma once
#include "cec/engine.hpp"
#include "esp_err.h"
#include <memory>

namespace cec {
struct Statistics { uint32_t transmitted = 0, received = 0, tx_errors = 0, rx_dropped = 0; };
class Bus {
public:
    Bus();
    ~Bus();
    esp_err_t start(int gpio);
    void set_address(uint8_t address);
    // A bounded wait provides producer backpressure when the TX queue fills.
    bool send(const Frame &frame, uint32_t timeout_ms = 0);
    // Queue in FIFO order and wait for the requested wire attempts to finish. True means
    // admitted/completed, not acknowledged; failures are counted in statistics.
    bool send_and_wait(const Frame &frame, uint32_t timeout_ms = 0, unsigned attempts = 1);
    TxResult transmit(const Frame &frame, unsigned attempts = 3);
    bool receive(Frame &frame, uint32_t timeout_ms = 0);
    Statistics statistics() const;
    bool idle() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
