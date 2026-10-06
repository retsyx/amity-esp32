#pragma once

#include <bitset>
#include <cstdint>

namespace amity_matter {
constexpr uint16_t apple_home_vendor = 0x1349;
constexpr uint16_t apple_keychain_vendor = 0x1384;

// Track committed Home fabrics only: rolling back an incomplete Home pairing
// must not revoke the Keychain fabric that was used to attempt that pairing.
class AppleFabricCleanup {
public:
    void committed(uint8_t index, uint16_t vendor)
    {
        if (valid(index)) homes_.set(index, vendor == apple_home_vendor);
    }

    // Persist the cleanup intent before the last Home is deleted, so a reset
    // between its removal and the deferred cleanup does not leave an orphan.
    bool will_remove(uint8_t index)
    {
        if (!valid(index)) return false;
        removing_homes_.set(index, homes_.test(index));
        return homes_.test(index) && homes_.count() == 1;
    }

    bool removed(uint8_t index)
    {
        if (!valid(index)) return false;
        const bool home = removing_homes_.test(index);
        removing_homes_.reset(index);
        homes_.reset(index);
        if (home && homes_.none()) pending_ = true;
        return home && homes_.none();
    }

    void restore_pending(bool pending) { pending_ = pending; }
    bool pending() const { return pending_; }
    bool has_home() const { return homes_.any(); }
    void completed() { pending_ = false; }

private:
    static bool valid(uint8_t index) { return index != 0 && index != 255; }
    std::bitset<256> homes_;
    std::bitset<256> removing_homes_;
    bool pending_ = false;
};
}
