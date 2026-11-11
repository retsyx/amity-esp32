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
    bool operator==(const Activity& other)const{return name==other.name&&display==other.display&&source==other.source&&audio==other.audio&&switch_device==other.switch_device&&switch_input==other.switch_input&&activity_id==other.activity_id;}
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
    bool operator==(const Device& other)const{return name==other.name&&bus==other.bus&&address==other.address&&physical_address==other.physical_address&&type==other.type&&vendor_id==other.vendor_id&&resolved==other.resolved;}
};
struct Snapshot {
    uint64_t applied_config_revision=0,applied_presentation_revision=0;
    uint32_t processed_activity_selection=0;
    std::vector<Activity> activities;
    std::vector<Device> devices;
    int current_activity = -1;
    uint32_t activity_generation = 0;
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
// Playback consumers do not need copies of the activity list or device cache.
struct PlaybackState {
    uint64_t applied_config_revision=0,applied_presentation_revision=0;
    int current_activity=-1;
    system::ActivityId activity_id{};
    bool audio_status_known=false,muted=false;
    uint8_t volume=127;
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
// One adapter's device resolver and CEC translation, called by its owner task.
class Hub {
public:
    explicit Hub(Transport &transport) : transport_(transport) {}
    bool execute_owned(const system::OwnedIntent&);
    static bool source_input_announcement(const system::OwnedIntent&);
    bool source_input_announcement_here(const system::OwnedIntent&) const;
    bool announce_owned_input();
    void set_addresses(uint8_t front, uint8_t back, uint16_t physical = 0x1000);
    void announce(BusSide side);
    static bool audio_key(uint8_t key);
    // An omitted audio selection uses the display; a missing selected device does not.
    template<class T> static const T& audio_recipient(const T& audio,const T& display){
        return audio!=T{}?audio:display;
    }
    uint8_t translated_key(uint8_t key) const;
    bool transmit_key(const Device&,uint8_t mapped,bool audio);
    bool transmit_release(const Device&,bool audio);
    void key_released(bool audio);
    void owner_audio_held(bool held){owner_audio_held_=held;}
    void tick(uint64_t now_us);
    enum class ReceiveStep : uint8_t { Frame, Input, Switch, Announce, ForceBack, Done };
    uint8_t receive(BusSide side, const cec::Frame &frame, ReceiveStep step = ReceiveStep::Frame);
    ReceiveStep receive_step() const { return receive_step_; }
    void discovered(BusSide side, uint8_t address);
    void begin_discovery(uint8_t sides = 3);
    void determine_front_physical_address(uint16_t fallback);
    bool apply_default_front_physical_address(uint16_t fallback);
    std::optional<uint16_t> detected_front_physical_address() const { return detected_physical_; }
    void finish_discovery();
    // Retry the original operation when this opaque continuation is ready.
    // The resolver alone decides whether a missing recipient needs a scan.
    uint8_t pending_resolution() const { return pending_resolution_; }
    Device *find_device(const std::string &name, std::optional<BusSide> side = {});
    bool update_observation(system::OwnedAdapterSnapshot&)const;
    uint16_t front_physical_address()const{return physical_;}
    uint32_t activity_generation() const { return activity_generation_; }
private:
    BusSide lookup_side(const std::string &name) const;
    Device *discovery_device(BusSide side, uint8_t address);
    void resolve_device(BusSide side, uint8_t address, bool restore_route);
    void invalidate_endpoint(BusSide side, uint8_t address);
    Activity current() const;
    std::string key_name(uint8_t key) const;
    bool send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data);
    bool power(const Device &device, bool on);
    void input(const Activity &activity);
    void fallback_input(const Activity&);
    void active_source();
    void identity(BusSide side, const cec::Frame &frame);
    void source_thief(const cec::Frame&);
    uint8_t resume_received();
    void request_audio_status();
    bool recipient_held(const Device&);
    bool finite_press(const Device&, const uint8_t*, size_t);
    Transport &transport_;
    uint64_t owned_generation_ = 1;
    std::optional<BusSide> owner_source_side_, owner_audio_side_;
    Activity context_;
    std::vector<Device> devices_;
    std::vector<Device> discovery_;
    bool discovering_ = false;
    uint8_t pending_resolution_ = 0;
    ReceiveStep receive_step_ = ReceiveStep::Frame;
    int selected_ = -1;
    uint32_t activity_generation_ = 0;
    uint8_t front_ = 15, back_ = 15;
    uint16_t physical_ = 0xffff, discovered_physical_ = 0xffff;
    std::optional<uint16_t> detected_physical_;
    bool playing_ = false, owner_audio_held_ = false;
    bool audio_query_pending_ = false;
    uint64_t now_us_ = 0, audio_query_due_ = 0;
    bool audio_status_known_ = false, muted_ = false;
    uint8_t volume_ = 127;
};
// The one activity sequence used by every Amity. It dispatches typed actions;
// each receiving adapter resolves the recipient and submits the CEC frame.
class ActivityHub {
public:
    explicit ActivityHub(Transport &transport) : transport_(transport) {}
    using ActionReady = std::function<void()>;
    // Saved edits retain the selected recipe and its execution state.
    bool configure_scoped(std::vector<Activity> activities);
    using OwnedSender = std::function<system::OwnedDispatchResult(const system::OwnedIntent&)>;
    // Dispatch may stamp the request synchronously. Retained work owns a copy.
    using OwnedDispatcher = std::function<system::OwnedDispatchResult(system::OwnedIntent&)>;
    void scoped_transport(std::vector<system::ScopedSelector>, OwnedDispatcher,
                          std::optional<system::CecPortRef> default_port = {},bool publish_context=true);
    void scoped_owner(system::OctopusId owner){scoped_owner_=owner;}
    void scoped_ports(std::vector<system::CecPortRef> ports){scoped_owner_ports_=std::move(ports);}
    int stage_activity(Activity,std::vector<system::ScopedSelector>,bool selected,uint64_t generation,bool reselect);
    void selection_generation(uint64_t generation){next_owned_generation_=generation;}
    uint64_t scoped_generation() const { return owned_generation_; }
    void owned_step_result(const system::OwnedIntent&,const system::AdmissionResult&);
    bool refresh_owned_context(system::OctopusId owner){return publish_owned_context(false,owner);}
    void scoped_audio_status(bool known, bool muted, uint8_t volume);
    bool select_activity(int index, ActionReady ready = {});
    bool press_key(uint8_t key, bool repeat = false);
    std::optional<system::QualifiedDeviceName> key_target(uint8_t key) const;
    bool release_key(uint8_t key);
    bool release_key();
    bool standby(ActionReady ready = {});
    bool activity_pending() const { return activity_execution_.has_value(); }
    void cancel_activity(){activity_execution_.reset();next_owned_generation_.reset();}
    Snapshot snapshot() const;
    bool update_snapshot(Snapshot&)const;
    struct AudioStatus {bool known,muted;uint8_t volume;};
    AudioStatus audio_status()const{return {audio_status_known_,muted_,volume_};}
    int current_activity()const{return selected_;}
    size_t activity_count()const{return activities_.size();}
    uint32_t activity_generation() const { return activity_generation_; }
private:
    Activity current() const;
    std::string key_name(uint8_t key) const;
    void active_source(const std::string& display);
    void request_audio_status();
    std::optional<system::QualifiedDeviceName> target(const std::string&) const;
    bool owned(system::OwnedIntentKind, const std::string&, bool on = false, uint8_t value = 0);
    bool publish_owned_context(bool source_changed,system::OctopusId owner={});
    Transport &transport_;
    std::vector<system::ScopedSelector> scoped_selectors_;
    OwnedDispatcher owned_sender_ = [](system::OwnedIntent&){return system::OwnedDispatchResult{};};
    std::optional<system::CecPortRef> scoped_default_port_;
    system::OctopusId scoped_owner_;
    std::optional<uint64_t> next_owned_generation_;
    std::vector<system::CecPortRef> scoped_owner_ports_;
    uint64_t owned_generation_ = 1;
    std::vector<Activity> activities_;
    // Local variables survive lookup/TX completion. This is neither a second
    // activity plan nor an outgoing CEC frame queue.
    struct ActivityExecution {
        enum class Step { Release, Off, On, Source, Switch, Announce, Commit, Audio, ForceOwned, Finish };
        Activity before,after;std::vector<std::string> before_names,after_names;
        int target=-1;size_t cursor=0;Step step=Step::Off;bool repair=false,waiting_tx=false,source_started=false,source_done=false,switch_started=false,switch_done=false;
        std::optional<bool> source_present;system::AdmissionCode switch_result=system::AdmissionCode::accepted;std::vector<system::CecPortRef> force_ports;
    };
    std::optional<ActivityExecution> activity_execution_;
    int selected_ = -1;
    uint32_t activity_generation_ = 0;
    bool audio_status_known_ = false, muted_ = false;
    uint8_t volume_ = 127;
};
}
