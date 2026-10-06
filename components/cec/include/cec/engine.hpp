#pragma once
#include <cstddef>
#include <cstdint>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CEC_IRAM IRAM_ATTR
#else
#define CEC_IRAM
#endif

namespace cec {
struct Frame {
    uint8_t data[16]{};
    uint8_t size = 0;
    uint8_t source() const { return data[0] >> 4; }
    uint8_t destination() const { return data[0] & 15; }
};
enum class TxResult : uint8_t { Pending, Success, Nack, ArbitrationLost, LineError, Timeout };

// Allocation-free wire state machine. Call tick at <= 50 us intervals. All
// timestamps are monotonic microseconds. The returned value means pull LOW;
// false always means release the open-drain output, never drive it high.
class Engine {
public:
    void set_address(uint8_t address) { address_ = address < 15 ? address : 15; }
    bool begin(const Frame &frame, uint64_t now, bool retry = false);
    bool CEC_IRAM tick(uint64_t now, bool line_high);
    bool CEC_IRAM take_received(Frame &frame);
    TxResult CEC_IRAM take_result();
    bool busy() const { return pending_ || transmitting_; }
private:
    void CEC_IRAM fail(TxResult result);
    void CEC_IRAM receive_tick(uint64_t now, bool high);
    uint8_t address_ = 15;
    bool last_high_ = true;
    uint64_t last_tick_ = 0, falling_ = 0, high_since_ = 0;
    bool receiving_ = false, start_seen_ = false, sampled_ = true, self_rx_ = false;
    bool ack_drive_ = false, eom_ = false, rx_ready_ = false;
    uint8_t rx_bit_ = 0, rx_byte_ = 0;
    uint64_t ack_until_ = 0;
    Frame rx_{}, received_{};
    Frame tx_{};
    bool pending_ = false, transmitting_ = false, tx_sampled_ = false;
    uint64_t requested_ = 0, tx_start_ = 0;
    uint32_t free_time_ = 12000;
    int tx_slot_ = -1;
    TxResult result_ = TxResult::Pending;
};
}
