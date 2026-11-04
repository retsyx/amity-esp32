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
#include "amity/system/execution.hpp"
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace amity {
enum class BusSide : uint8_t { Front, Back };
struct Activity {
    std::string name, display, source, audio, switch_device;
    uint8_t switch_input = 0;
    system::ActivityId activity_id{};
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
    // A failed recipient lookup must not enumerate the other isolated wire.
    virtual bool request_scan(BusSide) { request_scan(); return false; }
    virtual bool activity_step_pending() const { return false; }
    // Fence queued wire work before an endpoint can identify another appliance.
    virtual void invalidate_binding(BusSide, uint8_t) {}
};
// Called from one owner task. There are no ESP-IDF dependencies in this class.
class Hub {
public:
    explicit Hub(Transport &transport) : transport_(transport) {}
    using ActionReady = std::function<void()>;
    bool configure(std::vector<Activity> activities, ActionReady ready = {});
    // Saved edits retain the selected recipe and its execution state.
    bool configure_scoped(std::vector<Activity> activities);
    using OwnedSender = std::function<system::OwnedDispatchResult(const system::OwnedIntent&)>;
    // Dispatch stamps the caller's request synchronously; senders only read it.
    // A callback retaining work beyond the call must own its own copy.
    using OwnedDispatcher = std::function<system::OwnedDispatchResult(system::OwnedIntent&)>;
    using OwnedLookup = std::function<std::optional<Device>(const system::QualifiedDeviceName&)>;
    void scoped_transport(std::vector<system::ScopedSelector>, OwnedDispatcher, OwnedLookup,
                          std::optional<system::CecPortRef> default_port = {},bool publish_context=true);
    bool scoped() const { return bool(owned_sender_); }
    void scoped_owner(system::OctopusId owner){scoped_owner_=owner;}
    void scoped_ports(std::vector<system::CecPortRef> ports){scoped_owner_ports_=std::move(ports);}
    // Install one transient owner-local selection without configure's cleanup.
    // The previous effective activity remains the recipe's before state.
    int stage_activity(Activity,std::vector<system::ScopedSelector>,bool selected,uint64_t generation);
    void selection_generation(uint64_t generation){next_owned_generation_=generation;}
    uint64_t scoped_generation() const { return owned_generation_; }
    // Owner-local actions reuse the same original translation and held state.
    bool execute_owned(const system::OwnedIntent&);
    static bool source_input_announcement(const system::OwnedIntent&);
    bool source_input_announcement_here(const system::OwnedIntent&) const;
    // The owner completes this before the source's ordinary Set Stream Path.
    bool announce_owned_input();
    void owned_step_result(const system::OwnedIntent&,const system::AdmissionResult&);
    bool refresh_owned_context(system::OctopusId owner){return publish_owned_context(false,owner);}
    void qualified_names(bool value=true){qualified_names_=value;}
    void scoped_audio_status(bool known, bool muted, uint8_t volume);
    void set_addresses(uint8_t front, uint8_t back, uint16_t physical = 0x1000);
    void announce(BusSide side);
    bool select_activity(int index, ActionReady ready = {});
    bool press_key(uint8_t key, bool repeat = false);
    static bool audio_key(uint8_t key);
    // An omitted audio selection uses the display; a missing selected device does not.
    template<class T> static const T& audio_recipient(const T& audio,const T& display){
        return audio!=T{}?audio:display;
    }
    std::optional<system::QualifiedDeviceName> key_target(uint8_t key) const;
    std::string key_name(uint8_t key) const;
    uint8_t translated_key(uint8_t key) const;
    bool transmit_key(const Device&,uint8_t mapped,bool audio);
    bool transmit_release(const Device&,bool audio);
    void key_released(bool audio);
    void owner_audio_held(bool held){owner_audio_held_=held;}
    // Admit the operand-free final release when this recipient's last held
    // key ends. Releasing one raw key never changes another key's state.
    bool release_key(uint8_t key);
    // Activity changes and explicit global cancellation release every key.
    bool release_key();
    void tick(uint64_t now_us);
    bool standby(ActionReady ready = {});
    void force_standby();
    void fix_current_activity();
    enum class ReceiveStep : uint8_t { Frame, Input, Switch, Announce, ForceBack, Done };
    uint8_t receive(BusSide side, const cec::Frame &frame, ReceiveStep step = ReceiveStep::Frame);
    ReceiveStep receive_step() const { return receive_step_; }
    void discovered(BusSide side, uint8_t address);
    void clear_devices();
    void begin_discovery(uint8_t sides = 3);
    void determine_front_physical_address(uint16_t fallback);
    bool apply_default_front_physical_address(uint16_t fallback);
    std::optional<uint16_t> detected_front_physical_address() const { return detected_physical_; }
    void finish_discovery();
    // Callers submit the same operation again only when this opaque lookup
    // continuation is ready. Cache access and scan throttling stay in get_device.
    uint8_t pending_resolution() const { return pending_resolution_; }
    bool activity_pending() const { return activity_execution_.has_value(); }
    void cancel_activity(){activity_execution_.reset();next_owned_generation_.reset();}
    Device *find_device(const std::string &name, std::optional<BusSide> side = {});
    Snapshot snapshot() const;
    uint32_t activity_generation() const { return activity_generation_; }
private:
    BusSide lookup_side(const std::string &name) const;
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
    uint8_t resume_received();
    void request_audio_status();
    bool audio_key_held() const;
    bool press_device(uint8_t key, bool repeat);
    bool recipient_held(const Device&,std::optional<uint8_t> ending_key = {});
    bool finite_press(const Device&, const uint8_t*, size_t);
    std::optional<system::QualifiedDeviceName> target(const std::string&) const;
    bool owned(system::OwnedIntentKind, const std::string&, bool on = false, uint8_t value = 0);
    bool publish_owned_context(bool source_changed,system::OctopusId owner={});
    Transport &transport_;
    std::vector<system::ScopedSelector> scoped_selectors_;
    OwnedDispatcher owned_sender_;
    OwnedLookup owned_lookup_;
    std::optional<system::CecPortRef> scoped_default_port_;
    system::OctopusId scoped_owner_;std::optional<uint64_t> next_owned_generation_;
    std::vector<system::CecPortRef> scoped_owner_ports_;
    bool qualified_names_=false;
    Device scoped_device_;
    uint64_t owned_generation_ = 1;
    std::optional<BusSide> owner_source_side_, owner_audio_side_;
    std::vector<Activity> activities_;
    // The original activity function's local variables survive an async
    // device lookup/TX wait. No CEC frame or executable plan is retained.
    struct ActivityExecution {
        enum class Step { Release, Off, On, Source, Switch, Announce, Commit, Audio, ForceFront, ForceBack, ForceOwned, Finish };
        Activity before,after;std::vector<std::string> before_names,after_names;
        int target=-1;size_t cursor=0;Step step=Step::Off;bool repair=false,waiting_tx=false,source_started=false,source_done=false,switch_started=false,switch_done=false;
        std::optional<bool> source_present;system::AdmissionCode switch_result=system::AdmissionCode::accepted;std::vector<system::CecPortRef> force_ports;
    };
    std::optional<ActivityExecution> activity_execution_;
    std::vector<Device> devices_;
    std::vector<Device> discovery_;
    bool discovering_ = false;
    uint8_t pending_resolution_ = 0;
    ReceiveStep receive_step_ = ReceiveStep::Frame;
    int selected_ = -1;
    uint32_t activity_generation_ = 0;
    uint32_t binding_generation_ = 0, uncertain_releases_ = 0;
    uint8_t front_ = 15, back_ = 15;
    uint16_t physical_ = 0xffff, discovered_physical_ = 0xffff;
    std::optional<uint16_t> detected_physical_;
    bool playing_ = false;
    bool owner_audio_held_ = false;
    struct HeldKey {
        uint8_t raw_key, key;
        bool audio;
    };
    std::vector<HeldKey> held_keys_;
    bool audio_query_pending_ = false;
    uint64_t now_us_ = 0, audio_query_due_ = 0;
    bool audio_status_known_ = false, muted_ = false;
    uint8_t volume_ = 127;
};
}
