/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/hub.hpp"
#include <algorithm>
#include <vector>
#include <utility>

namespace amity {
// Owner/admission callers serialize access. A reservation starts before command
// enqueue, so requests made while setup is queued cannot slip through the guard.
class ActivitySettling {
public:
    struct Peer { BusSide side; uint8_t address; uint16_t physical; };
    explicit ActivitySettling(uint32_t seconds) : quiet_us_(uint64_t(seconds) * 1000000) {}
    static constexpr uint64_t reply_timeout_us = 2000000;
    void reserve(int target) {
        target_ = target; active_ = quiet_us_ != 0; preparing_ = active_;
        peers_.clear(); replies_.clear(); quiet_since_ = reply_deadline_ = 0;
    }
    void start(int target, std::vector<Peer> peers) { reserve(target); peers_ = std::move(peers); }
    void finish(uint64_t now) {
        if (!active_) return;
        preparing_ = false; quiet_since_ = now; reply_deadline_ = now + reply_timeout_us;
    }
    void cancel() { active_ = preparing_ = false; peers_.clear(); replies_.clear(); }
    int target() const { return target_; }
    bool active(uint64_t now) {
        if (!active_ || preparing_) return active_;
        if (!replies_.empty() && now >= reply_deadline_) {
            replies_.clear(); quiet_since_ = std::max(quiet_since_, reply_deadline_);
        }
        if (replies_.empty() && now >= quiet_since_ && now - quiet_since_ >= quiet_us_) active_ = false;
        return active_;
    }
    void expect(BusSide side, uint8_t destination, const uint8_t *data, size_t size) {
        if (!active_ || !preparing_ || !size) return;
        uint8_t response = 0, minimum = 0; bool broadcast = false;
        switch (data[0]) {
        case 0x71: response = 0x7a; minimum = 3; break;
        case 0x8f: response = 0x90; minimum = 3; break;
        case 0x46: response = 0x47; minimum = 3; break;
        case 0x9f: response = 0x9e; minimum = 3; break;
        case 0x83: response = 0x84; minimum = 5; broadcast = true; break;
        case 0x8c: response = 0x87; minimum = 5; broadcast = true; break;
        case 0xa5: response = 0xa6; minimum = 6; broadcast = true; break;
        case 0x86: if (size < 3) return; response = 0x82; minimum = 4; broadcast = true; break;
        default: return; // Power/standby and user-control commands only require ACK.
        }
        for (const auto &peer : peers_) {
            if (peer.side != side) continue;
            const bool route = data[0] == 0x86;
            if (route ? peer.physical != uint16_t((data[1] << 8) | data[2]) : peer.address != destination) continue;
            const Reply reply{peer, data[0], response, minimum, broadcast, route};
            const auto duplicate = std::find_if(replies_.begin(), replies_.end(), [&](const Reply &r) {
                return r.peer.side == side && r.peer.address == peer.address && r.request == data[0];
            });
            if (duplicate == replies_.end()) replies_.push_back(reply);
        }
    }
    void received(BusSide side, const cec::Frame &frame, uint8_t own, uint64_t now) {
        // Replies can arrive while other setup messages are still awaiting TX.
        // Collect them immediately; finish() starts quiet time after the final
        // admitted setup message completes.
        if (!active(now) || frame.size < 2 || frame.size > 16 || frame.source() >= 15) return;
        const auto old_size = replies_.size();
        replies_.erase(std::remove_if(replies_.begin(), replies_.end(), [&](const Reply &r) {
            if (r.peer.side != side || r.peer.address != frame.source()) return false;
            if (frame.data[1] == 0x00) // Feature Abort resolves only its own pending request.
                return frame.size >= 4 && frame.destination() == own && frame.data[2] == r.request;
            if (frame.data[1] != r.response || frame.size < r.minimum ||
                frame.destination() != (r.broadcast ? 15 : own)) return false;
            return !r.route || uint16_t((frame.data[2] << 8) | frame.data[3]) == r.peer.physical;
        }), replies_.end());
        if (replies_.size() != old_size) quiet_since_ = now;
    }
private:
    struct Reply { Peer peer; uint8_t request, response, minimum; bool broadcast, route; };
    uint64_t quiet_us_, quiet_since_ = 0, reply_deadline_ = 0;
    int target_ = -1;
    bool active_ = false, preparing_ = false;
    std::vector<Peer> peers_;
    std::vector<Reply> replies_;
};
}
