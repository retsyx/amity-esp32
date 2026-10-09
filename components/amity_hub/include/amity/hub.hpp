/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "cec/engine.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace amity {
enum class BusSide : uint8_t { Front, Back };
struct Activity {
    std::string name, display, source, audio, switch_device;
    uint8_t switch_input = 0;
};
struct Device {
    std::string name;
    BusSide bus = BusSide::Back;
    uint8_t address = 15;
    uint16_t physical_address = 0xffff;
    uint8_t type = 0xff;
    uint32_t vendor_id = 0;
};
struct Quirk {
    uint32_t vendor_id = 0;
    std::vector<uint8_t> power_on, power_off;
};
struct Snapshot {
    std::vector<Activity> activities;
    std::vector<Device> devices;
    int current_activity = -1;
    uint32_t activity_generation = 0;
    uint32_t discovery_generation = 0;
    uint8_t front_address = 15, back_address = 15;
    bool ready = false;
    bool discovering = false;
    bool activity_settling = false;
    bool audio_status_known = false;
    bool muted = false;
    uint8_t volume = 127; // CEC 0..100, 127 means unknown/unsupported.
    uint32_t tx_errors = 0, rx_dropped = 0;
};
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(BusSide side, uint8_t destination, const uint8_t *data, size_t size) = 0;
    virtual void request_scan() = 0;
};
// Called from one owner task. There are no ESP-IDF dependencies in this class.
class Hub {
public:
    explicit Hub(Transport &transport) : transport_(transport) {}
    void configure(std::vector<Activity> activities, std::vector<Quirk> quirks = {});
    void set_addresses(uint8_t front, uint8_t back, uint16_t physical = 0x1000);
    void announce(BusSide side);
    bool select_activity(int index);
    bool press_key(uint8_t key, bool repeat = false);
    bool release_key();
    void tick(uint64_t now_us);
    void standby();
    void force_standby();
    void fix_current_activity();
    void receive(BusSide side, const cec::Frame &frame);
    void discovered(BusSide side, uint8_t address);
    void clear_devices();
    void begin_discovery();
    void finish_discovery();
    bool activity_needs_discovery(int index) const;
    bool key_needs_discovery(uint8_t key) const;
    Snapshot snapshot() const;
    uint32_t activity_generation() const { return activity_generation_; }
private:
    Device *find(const std::string &name);
    const Device *known(const std::string &name) const;
    Device *discovery_device(BusSide side, uint8_t address);
    void resolve_device(BusSide side, uint8_t address, bool restore_route);
    Activity current() const;
    bool send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data);
    void power(const Device &device, bool on, bool quirk = true);
    void input(const Activity &activity);
    void active_source();
    void identity(BusSide side, const cec::Frame &frame);
    void source_thief(const cec::Frame &frame);
    void request_audio_status();
    Transport &transport_;
    std::vector<Activity> activities_;
    std::vector<Device> devices_;
    std::vector<Device> discovery_;
    bool discovering_ = false;
    std::vector<Quirk> quirks_;
    int selected_ = -1;
    uint32_t activity_generation_ = 0;
    uint8_t front_ = 15, back_ = 15;
    uint16_t physical_ = 0x1000;
    bool playing_ = false;
    bool key_held_ = false;
    bool held_audio_ = false;
    bool release_pending_ = false;
    uint16_t pending_releases_[2] = {};
    bool audio_query_pending_ = false;
    uint64_t now_us_ = 0, audio_query_due_ = 0;
    bool audio_status_known_ = false, muted_ = false;
    uint8_t volume_ = 127;
    BusSide held_bus_ = BusSide::Back;
    uint8_t held_address_ = 15;
    uint8_t held_raw_key_ = 0xff, held_key_ = 0xff;
};
}
