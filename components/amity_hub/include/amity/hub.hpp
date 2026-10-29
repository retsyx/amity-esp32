/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "cec/engine.hpp"
#include "amity/system/owned_intent.hpp"
#include <functional>
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
    // The original owner-local cache has a name for this appliance.
    bool resolved = false;
};
struct Snapshot {
    uint64_t applied_config_revision=0,applied_presentation_revision=0;
    uint32_t processed_activity_selection=0;
    std::vector<Activity> activities;
    std::vector<Device> devices;
    int current_activity = -1;
    uint32_t activity_generation = 0;
    uint32_t binding_generation = 0, uncertain_releases = 0;
    std::string activity_error;
    uint32_t discovery_generation = 0;
    uint8_t front_address = 15, back_address = 15;
    uint16_t front_physical_address = 0xffff; // Unknown until the startup back-bus scan completes.
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
    // Fence queued wire work before an endpoint can identify another appliance.
    virtual void invalidate_binding(BusSide, uint8_t) {}
};
// Called from one owner task. There are no ESP-IDF dependencies in this class.
class Hub {
public:
    explicit Hub(Transport &transport) : transport_(transport) {}
    void configure(std::vector<Activity> activities);
    using OwnedSender = std::function<system::OwnedDispatchResult(const system::OwnedIntent&)>;
    // Dispatch stamps the caller's request synchronously; senders only read it.
    // A callback retaining work beyond the call must own its own copy.
    using OwnedDispatcher = std::function<system::OwnedDispatchResult(system::OwnedIntent&)>;
    using OwnedLookup = std::function<std::optional<Device>(const system::QualifiedDeviceName&)>;
    void scoped_transport(std::vector<system::ScopedSelector>, OwnedDispatcher, OwnedLookup,
                          std::optional<system::CecPortRef> default_port = {});
    bool scoped() const { return bool(owned_sender_); }
    uint64_t scoped_generation() const { return owned_generation_; }
    bool input_pending()const{return !pending_inputs_.empty();}
    bool held_recipient(uint8_t key,BusSide side,uint8_t address)const;
    // Owner-local actions reuse the same original translation and held state.
    bool execute_owned(const system::OwnedIntent&);
    bool owned_target_available(const system::QualifiedDeviceName&);
    void owned_input_result(system::OctopusId,uint64_t generation,uint64_t sequence,bool source_present);
    void fence_owned_owner(system::OctopusId);
    bool refresh_owned_context(system::OctopusId owner){return publish_owned_context(false,owner);}
    void qualified_names(bool value=true){qualified_names_=value;}
    void scoped_audio_status(bool known, bool muted, uint8_t volume);
    void set_addresses(uint8_t front, uint8_t back, uint16_t physical = 0x1000);
    void announce(BusSide side);
    bool select_activity(int index);
    bool press_key(uint8_t key, bool repeat = false);
    // Admit the operand-free final release when this recipient's last held
    // key ends. Releasing one raw key never changes another key's state.
    bool release_key(uint8_t key);
    // Activity changes and explicit global cancellation release every key.
    bool release_key();
    void tick(uint64_t now_us);
    void standby();
    void force_standby();
    void fix_current_activity();
    void receive(BusSide side, const cec::Frame &frame);
    void discovered(BusSide side, uint8_t address);
    void clear_devices();
    void begin_discovery();
    void determine_front_physical_address(uint16_t fallback);
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
    void invalidate_endpoint(BusSide side, uint8_t address);
    Activity current() const;
    bool send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data);
    bool power(const Device &device, bool on);
    void input(const Activity &activity);
    void fallback_input(const Activity&);
    void active_source(const std::string& display = {});
    void identity(BusSide side, const cec::Frame &frame);
    void source_thief(const cec::Frame&);
    void request_audio_status(bool resolved_input = false);
    bool audio_key_held() const;
    Device *find_on_side(BusSide, const std::string&);
    bool press_device(const Device*, uint8_t key, bool repeat);
    bool finite_press(const Device&, const uint8_t*, size_t);
    std::optional<system::QualifiedDeviceName> target(const std::string&) const;
    bool owned(system::OwnedIntentKind, const std::string&, bool on = false, uint8_t value = 0, bool repeat = false);
    bool publish_owned_context(bool source_changed,system::OctopusId owner={});
    Transport &transport_;
    std::vector<system::ScopedSelector> scoped_selectors_;
    OwnedDispatcher owned_sender_;
    OwnedLookup owned_lookup_;
    std::optional<system::CecPortRef> scoped_default_port_;
    struct PendingInput {Activity activity;system::OctopusId owner;uint64_t generation=0,sequence=0;bool query_audio=false;};
    std::vector<PendingInput> pending_inputs_;
    uint64_t current_input_sequence_=0;
    bool qualified_names_=false;
    mutable Device scoped_device_;
    uint64_t owned_generation_ = 1;
    std::optional<BusSide> owner_source_side_, owner_audio_side_;
    std::vector<Activity> activities_;
    std::vector<Device> devices_;
    std::vector<Device> discovery_;
    bool discovering_ = false;
    int selected_ = -1;
    std::optional<int> lookup_previous_selection_;
    uint32_t lookup_previous_generation_=0;
    uint32_t activity_generation_ = 0;
    uint32_t binding_generation_ = 0, uncertain_releases_ = 0;
    uint8_t front_ = 15, back_ = 15;
    uint16_t physical_ = 0xffff, discovered_physical_ = 0xffff;
    bool playing_ = false;
    struct HeldKey {
        uint8_t raw_key, key;
        BusSide bus;
        uint8_t address;
        bool audio;
        std::string selector{};
    };
    std::vector<HeldKey> held_keys_;
    bool audio_query_pending_ = false;
    uint64_t now_us_ = 0, audio_query_due_ = 0;
    bool audio_status_known_ = false, muted_ = false;
    uint8_t volume_ = 127;
};
}
