/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub.hpp"
#include <algorithm>
#include <utility>

namespace amity {
namespace {
bool usable_front_physical_address(uint16_t address) {
    // Zero belongs to the root TV; FFFF is the CEC invalid/unknown sentinel.
    if (address == 0 || address == 0xffff) return false;
    bool zero = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const auto port = (address >> shift) & 15;
        if (zero && port) return false;
        if (!port) zero = true;
    }
    return true;
}
std::vector<std::string> names(const Activity &a) {
    std::vector<std::string> result;
    // A device can fill several roles, but each power transition applies once.
    for (const auto &name : {a.display, a.source, a.audio, a.switch_device})
        if (!name.empty() && std::find(result.begin(), result.end(), name) == result.end()) result.push_back(name);
    return result;
}
}
void Hub::configure(std::vector<Activity> activities) {
    if (selected_ >= 0) standby();
    ++activity_generation_; ++owned_generation_; pending_inputs_.clear();lookup_previous_selection_.reset();
    activities_ = std::move(activities);
    if (owned_sender_) publish_owned_context(true);
}
void Hub::scoped_transport(std::vector<system::ScopedSelector> selectors, OwnedSender sender, OwnedLookup lookup,
        std::optional<system::CecPortRef> default_port) {
    scoped_selectors_ = std::move(selectors); owned_sender_ = std::move(sender); owned_lookup_ = std::move(lookup);
    scoped_default_port_ = std::move(default_port);
    if(owned_sender_)publish_owned_context(true);
}
std::optional<system::QualifiedDeviceName> Hub::target(const std::string& value) const {
    for (const auto& selector : scoped_selectors_) if (selector.value == value) return selector.target;
    return {};
}
bool Hub::owned(system::OwnedIntentKind kind, const std::string& value, bool on, uint8_t data, bool repeat) {
    if (!owned_sender_) return false;
    auto endpoint = target(value); if (!endpoint) return false;
    system::OwnedIntent intent; intent.kind = kind; intent.target = *endpoint; intent.generation = owned_generation_;
    intent.on = on; intent.key = data; intent.input = data; intent.repeat = repeat;
    return bool(owned_sender_(std::move(intent)));
}
bool Hub::publish_owned_context(bool source_changed,system::OctopusId owner) {
    if (!owned_sender_) return false;
    bool admitted=true;
    const auto activity = current(); std::vector<system::CecPortRef> ports;
    for (const auto& selector : scoped_selectors_) {
        if(owner&&selector.target.port.octopus_id!=owner)continue;
        auto port = selector.target.port; port.side = system::Side::front;
        if (std::find(ports.begin(), ports.end(), port) != ports.end()) continue;
        ports.push_back(port); system::OwnedIntent intent; intent.kind = system::OwnedIntentKind::context;
        intent.target.port = port; intent.generation = owned_generation_; intent.selected = selected_ >= 0;
        intent.source_changed = source_changed; intent.source = target(activity.source); intent.audio = target(activity.audio);
        intent.display = target(activity.display); intent.switch_device = target(activity.switch_device); intent.input = activity.switch_input;
        admitted=bool(owned_sender_(std::move(intent)))&&admitted;
    }
    return admitted;
}
void Hub::scoped_audio_status(bool known, bool muted, uint8_t volume) {
    audio_status_known_ = known; muted_ = muted; volume_ = volume;
}
Device *Hub::find_on_side(BusSide side, const std::string& name) {
    if(!name.empty())for(auto& device:devices_)if(device.bus==side&&device.name==name)return &device;
    return nullptr;
}
bool Hub::owned_target_available(const system::QualifiedDeviceName& name) {
    return find_on_side(name.port.side==system::Side::front?BusSide::Front:BusSide::Back,name.osd_name)!=nullptr;
}
void Hub::owned_input_result(system::OctopusId owner,uint64_t generation,uint64_t sequence,bool present) {
    const auto found=std::find_if(pending_inputs_.begin(),pending_inputs_.end(),[&](const auto& pending){return pending.owner==owner&&pending.generation==generation&&pending.sequence==sequence;});
    if(found==pending_inputs_.end()||generation!=owned_generation_)return;
    const auto pending=*found;pending_inputs_.erase(found);
    if(!present)fallback_input(pending.activity);
    if(pending.query_audio)request_audio_status(true);
    if(pending_inputs_.empty())lookup_previous_selection_.reset();
}
void Hub::fence_owned_owner(system::OctopusId owner){
    pending_inputs_.erase(std::remove_if(pending_inputs_.begin(),pending_inputs_.end(),[&](const auto& pending){return pending.owner==owner;}),pending_inputs_.end());
    if(pending_inputs_.empty())lookup_previous_selection_.reset();
}
bool Hub::execute_owned(const system::OwnedIntent& intent) {
    const auto side = intent.target.port.side == system::Side::front ? BusSide::Front : BusSide::Back;
    if (intent.kind == system::OwnedIntentKind::context) {
        const auto previous_source=current().source;const auto previous_side=owner_source_side_;
        Activity context; owner_source_side_.reset(); owner_audio_side_.reset();
        if(intent.source)context.source="@foreign:"+intent.source->port.octopus_id.str()+":"+intent.source->osd_name;
        if (intent.source && intent.source->port.octopus_id == intent.target.port.octopus_id && intent.source->port.tentacle_id == intent.target.port.tentacle_id) {
            context.source = intent.source->osd_name; owner_source_side_ = intent.source->port.side == system::Side::front ? BusSide::Front : BusSide::Back;
        }
        if (intent.audio && intent.audio->port.octopus_id == intent.target.port.octopus_id && intent.audio->port.tentacle_id == intent.target.port.tentacle_id) {
            context.audio = intent.audio->osd_name; owner_audio_side_ = intent.audio->port.side == system::Side::front ? BusSide::Front : BusSide::Back;
        }
        if(intent.display&&intent.display->port.octopus_id==intent.target.port.octopus_id&&intent.display->port.tentacle_id==intent.target.port.tentacle_id)context.display=intent.display->osd_name;
        if(intent.switch_device&&intent.switch_device->port.octopus_id==intent.target.port.octopus_id&&intent.switch_device->port.tentacle_id==intent.target.port.tentacle_id){context.switch_device=intent.switch_device->osd_name;context.switch_input=intent.input;}
        activities_ = {context}; selected_ = intent.selected ? 0 : -1;
        if (intent.source_changed||previous_source!=context.source||previous_side!=owner_source_side_) playing_ = false;
        return true;
    }
    if (intent.kind == system::OwnedIntentKind::announce_active_source) { active_source(); return true; }
    if (intent.kind == system::OwnedIntentKind::standby_wire) return send(side,15,{0x36});
    if (intent.kind == system::OwnedIntentKind::key_up) return release_key(intent.key);
    auto* device = find_on_side(side,intent.target.osd_name); if (!device) { transport_.request_scan(); return false; }
    switch (intent.kind) {
    case system::OwnedIntentKind::power:
        return power(*device,intent.on);
    case system::OwnedIntentKind::source_input:
        return send(side,15,{0x86,static_cast<uint8_t>(device->physical_address>>8),static_cast<uint8_t>(device->physical_address)});
    case system::OwnedIntentKind::switch_input: {
        const uint8_t data[] = {0x44,0x69,intent.input}; return finite_press(*device,data,sizeof(data));
    }
    case system::OwnedIntentKind::query_audio: {
        const bool admitted = send(side,device->address,{0x71});
        audio_query_pending_ = !admitted; if (!admitted) audio_query_due_ = now_us_ + 500000;
        return admitted;
    }
    case system::OwnedIntentKind::key_down: return press_device(device,intent.key,intent.repeat);
    default: return false;
    }
}

void Hub::set_addresses(uint8_t front, uint8_t back, uint16_t physical) {
    front_ = front; back_ = back; physical_ = physical;
}
void Hub::announce(BusSide side) {
    const bool front = side == BusSide::Front;
    if ((front ? front_ : back_) == 15 || (front && physical_ == 0xffff)) return;
    const uint16_t pa = front ? physical_ : 0;
    // Linux announces features first for a CEC 2.0 adapter, then its physical
    // address and vendor. Preserve both payloads and ordering.
    send(side, 15, {0xa6, 6, static_cast<uint8_t>(front ? 0x10 : 0x80), 0, 0});
    send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)});
    send(side, 15, {0x87, 0, 0, 0});
}
Snapshot Hub::snapshot() const {
    Snapshot s;
    s.activities = activities_; s.devices = devices_; s.current_activity = lookup_previous_selection_.value_or(selected_);
    s.activity_generation = lookup_previous_selection_?lookup_previous_generation_:activity_generation_;
    s.binding_generation = binding_generation_; s.uncertain_releases = uncertain_releases_;
    s.front_address = front_; s.back_address = back_; s.front_physical_address = physical_;
    s.ready = front_ < 15 && back_ < 15 && physical_ != 0xffff;
    s.audio_status_known = audio_status_known_; s.muted = muted_; s.volume = volume_;
    return s;
}
Activity Hub::current() const {
    return selected_ >= 0 && static_cast<size_t>(selected_) < activities_.size() ? activities_[selected_] : Activity{};
}
Device *Hub::find(const std::string &name) {
    if (name.empty()) return nullptr;
    if (owned_sender_) {
        const auto selector = target(name); if(!selector){transport_.request_scan();return nullptr;}
        const auto device = owned_lookup_ ? owned_lookup_(*selector) : std::optional<Device>{};
        scoped_device_ = device.value_or(Device{});scoped_device_.name=name;
        scoped_device_.bus=selector->port.side==system::Side::front?BusSide::Front:BusSide::Back;return &scoped_device_;
    }
    if (owner_source_side_ && name == current().source) return find_on_side(*owner_source_side_,name);
    if (owner_audio_side_ && name == current().audio) return find_on_side(*owner_audio_side_,name);
    for (auto& device : devices_) if (device.name == name) return &device;
    transport_.request_scan(); return nullptr;
}
const Device *Hub::known(const std::string &name) const {
    if (owned_sender_) {
        const auto selector=target(name);if(!selector)return nullptr;
        const auto device=owned_lookup_?owned_lookup_(*selector):std::optional<Device>{};
        scoped_device_=device.value_or(Device{});scoped_device_.name=name;
        scoped_device_.bus=selector->port.side==system::Side::front?BusSide::Front:BusSide::Back;return &scoped_device_;
    }
    if (owner_source_side_ && name == current().source) return const_cast<Hub*>(this)->find_on_side(*owner_source_side_,name);
    if (owner_audio_side_ && name == current().audio) return const_cast<Hub*>(this)->find_on_side(*owner_audio_side_,name);
    if (!name.empty()) for (const auto& device : devices_) if (device.name == name) return &device;
    return nullptr;
}
bool Hub::send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data) {
    return transport_.send(side, dest, data.begin(), data.size());
}
bool Hub::activity_needs_discovery(int index) const {
    if (index < -1 || index >= static_cast<int>(activities_.size())) return false;
    for (const auto &name : names(current())) if (!name.empty() && !known(name)) return true;
    if (index >= 0) for (const auto &name : names(activities_[index])) if (!name.empty() && !known(name)) return true;
    return false;
}
bool Hub::key_needs_discovery(uint8_t key) const {
    if (key == 0x40) return activity_needs_discovery(-1);
    const auto a = current();
    const auto &name = ((key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66) ? a.audio : a.source;
    return !name.empty() && !known(name);
}
bool Hub::power(const Device &device, bool on) {
    if (owned_sender_) return owned(system::OwnedIntentKind::power,device.name,on);
    if (!on) return send(device.bus, device.address, {0x36});
    else if (device.type == 0) return send(device.bus, device.address, {0x04});
    else {
        const uint8_t data[] = {0x44,0x6d}; return finite_press(device,data,sizeof(data));
    }
}
bool Hub::finite_press(const Device& device, const uint8_t* data, size_t size) {
    if (!transport_.send(device.bus,device.address,data,size)) return false;
    const bool held = std::any_of(held_keys_.begin(),held_keys_.end(),[&](const auto& key){return key.bus == device.bus && key.address == device.address;});
    return held || send(device.bus,device.address,{0x45});
}
void Hub::active_source(const std::string& display) {
    if (owned_sender_) {
        const auto value=display.empty()?current().display:display;
        if(target(value))owned(system::OwnedIntentKind::announce_active_source,value);
        else if (scoped_default_port_) {
            system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::announce_active_source;
            intent.target.port=*scoped_default_port_;intent.generation=owned_generation_;owned_sender_(std::move(intent));
        }
        return;
    }
    if (physical_ == 0xffff) return;
    send(BusSide::Front, 15, {0x82, static_cast<uint8_t>(physical_ >> 8), static_cast<uint8_t>(physical_)});
}
void Hub::input(const Activity &activity) {
    if(owned_sender_)if(const auto source=target(activity.source)){
        current_input_sequence_=0;
        if(pending_inputs_.size()>=32)return;
        system::OwnedIntent intent;intent.kind=system::OwnedIntentKind::source_input;intent.target=*source;intent.generation=owned_generation_;
        const auto result=owned_sender_(std::move(intent));
        if(result.source_present){if(!*result.source_present)fallback_input(activity);}
        else if(result.admitted){pending_inputs_.push_back(PendingInput{activity,source->port.octopus_id,owned_generation_,result.sequence,false});current_input_sequence_=result.sequence;}
        return;
    }
    if (auto *d = find(activity.source)) {
        send(d->bus, 15, {0x86, static_cast<uint8_t>(d->physical_address >> 8), static_cast<uint8_t>(d->physical_address)});
        return;
    }
    fallback_input(activity);
}
void Hub::fallback_input(const Activity& activity){
    if (!activity.switch_device.empty()) {
        auto *d = find(activity.switch_device);
        if (!d) return;
        if (owned_sender_) owned(system::OwnedIntentKind::switch_input,activity.switch_device,false,activity.switch_input);
        else { const uint8_t data[] = {0x44,0x69,activity.switch_input}; finite_press(*d,data,sizeof(data)); }
    }
    active_source(activity.display);
}
bool Hub::select_activity(int index) {
    if (index < -1 || index >= static_cast<int>(activities_.size())) return false;
    if (index == selected_) { fix_current_activity(); return true; }
    release_key();
    pending_inputs_.clear();
    ++owned_generation_;
    const auto before_activity = current();
    const auto before = names(before_activity);
    const auto after_activity = index < 0 ? Activity{} : activities_[index];
    const auto after = names(after_activity);
    for (const auto &name : before) if (std::find(after.begin(), after.end(), name) == after.end()) {
        if (auto *d = find(name)) power(*d, false);
    }
    for (const auto &name : after) if (std::find(before.begin(), before.end(), name) == before.end()) {
        if (auto *d = find(name)) power(*d, true);
    }
    input(after_activity);
    if(input_pending()){lookup_previous_selection_=selected_;lookup_previous_generation_=activity_generation_;}
    selected_ = index;
    ++activity_generation_;
    audio_status_known_ = false; volume_ = 127;
    publish_owned_context(index < 0 || before_activity.source != after_activity.source);
    request_audio_status();
    if (index < 0 || before_activity.source != after_activity.source) playing_ = false;
    return true;
}
void Hub::fix_current_activity() {
    const auto a = current();
    for (const auto &name : names(a)) if (auto *d = find(name)) power(*d, true);
    input(a);
    request_audio_status();
}
void Hub::request_audio_status(bool resolved_input) {
    if(owned_sender_&&!resolved_input&&current_input_sequence_){
        for(auto& pending:pending_inputs_)if(pending.sequence==current_input_sequence_)pending.query_audio=true;
        current_input_sequence_=0;return;
    }
    const auto *audio = owner_audio_side_ ? find_on_side(*owner_audio_side_,current().audio) : known(current().audio);
    if (const auto *d = audio) {
        if (owned_sender_) { owned(system::OwnedIntentKind::query_audio,current().audio); return; }
        audio_query_pending_ = !send(d->bus, d->address, {0x71});
        if (audio_query_pending_) audio_query_due_ = now_us_ + 500000;
    } else audio_query_pending_ = false;
}
void Hub::standby() {
    if (selected_ < 0) force_standby();
    else select_activity(-1);
    playing_ = false;
}
void Hub::force_standby() {
    if (owned_sender_) {
        std::vector<system::CecPortRef> ports;
        for (const auto& selector : scoped_selectors_) for (const auto side : {system::Side::front,system::Side::back}) {
            auto port = selector.target.port; port.side = side;
            if (std::find(ports.begin(),ports.end(),port) != ports.end()) continue;
            ports.push_back(port); system::OwnedIntent intent; intent.kind = system::OwnedIntentKind::standby_wire;
            intent.target.port = port; intent.generation = owned_generation_; owned_sender_(std::move(intent));
        }
        return;
    }
    send(BusSide::Front, 15, {0x36});
    send(BusSide::Back, 15, {0x36});
}
bool Hub::press_key(uint8_t key, bool repeat) {
    const auto a = lookup_previous_selection_?(*lookup_previous_selection_>=0?activities_[*lookup_previous_selection_]:Activity{}):current();
    if (key == 0x40) { standby(); return true; }
    if (owned_sender_) {
        const auto held = std::find_if(held_keys_.begin(),held_keys_.end(),[&](const auto& h){return h.raw_key == key;});
        if (repeat && held != held_keys_.end()) return true;
        const bool audio = (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66;
        const auto selector = held != held_keys_.end() ? held->selector : audio ? a.audio : a.source;
        if (!known(selector) || !owned(system::OwnedIntentKind::key_down,selector,false,key,repeat)) return false;
        if (repeat) held_keys_.push_back({key,key,BusSide::Back,15,audio,selector});
        return true;
    }
    return press_device(nullptr,key,repeat);
}
bool Hub::press_device(const Device* supplied, uint8_t key, bool repeat) {
    const auto a = current();
    const uint8_t raw_key = key;
    const auto held = std::find_if(held_keys_.begin(), held_keys_.end(),
                                   [raw_key](const HeldKey &h) { return h.raw_key == raw_key; });
    // Default Python hub behavior emulates the play/pause toggle.
    if (key == 0x61) {
        if (held != held_keys_.end()) key = held->key;
        else key = playing_ ? 0x46 : 0x44;
    }
    const bool audio_key = (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66;
    // A held key keeps its accepted recipient even if discovery changes the
    // device cache before a repeat or release arrives.
    const auto *d = held == held_keys_.end() ? (supplied ? supplied : find(audio_key ? a.audio : a.source)) : nullptr;
    if (held == held_keys_.end() && !d) return false;
    const BusSide bus = held != held_keys_.end() ? held->bus : d->bus;
    const uint8_t address = held != held_keys_.end() ? held->address : d->address;
    if (!send(bus, address, {0x44, key})) return false;
    // Explicit transport commands and emulated toggles share the same last
    // accepted state. Held toggles keep emitting their original mapped key.
    if (key == 0x44) playing_ = true;
    else if (key == 0x46 || key == 0x45) playing_ = false;
    if (audio_key) audio_status_known_ = false;
    if (repeat) {
        if (held == held_keys_.end()) held_keys_.push_back({raw_key, key, bus, address, audio_key});
    } else {
        // A one-shot command cannot terminate any independently held key at
        // this recipient: User Control Released has no key operand.
        const bool recipient_held = std::any_of(held_keys_.begin(), held_keys_.end(),
            [bus, address](const HeldKey &h) { return h.bus == bus && h.address == address; });
        const bool admitted = recipient_held || send(bus, address, {0x45});
        if (audio_key && admitted) {
            audio_query_pending_ = true;
            audio_query_due_ = now_us_ + 500000;
        }
    }
    return true;
}
bool Hub::release_key(uint8_t key) {
    const auto held = std::find_if(held_keys_.begin(), held_keys_.end(),
                                   [key](const HeldKey &h) { return h.raw_key == key; });
    if (held == held_keys_.end()) return true;
    const HeldKey ended = *held;
    held_keys_.erase(held);
    if (owned_sender_) return owned(system::OwnedIntentKind::key_up,ended.selector,false,key);
    const bool recipient_held = std::any_of(held_keys_.begin(), held_keys_.end(),
        [&ended](const HeldKey &h) { return h.bus == ended.bus && h.address == ended.address; });
    const bool admitted = recipient_held || send(ended.bus, ended.address, {0x45});
    // The final-release decision is consumed even when the eight-slot driver FIFO
    // is full. No later tick, up or new key retries a dropped release.
    if (ended.audio && admitted) {
        audio_query_pending_ = true;
        audio_query_due_ = now_us_ + 500000;
    }
    if (!admitted) ++uncertain_releases_;
    return admitted;
}
bool Hub::release_key() {
    bool admitted = true;
    while (!held_keys_.empty()) {
        if (!release_key(held_keys_.front().raw_key)) admitted = false;
    }
    return admitted;
}
bool Hub::audio_key_held() const {
    return std::any_of(held_keys_.begin(), held_keys_.end(), [](const HeldKey &h) { return h.audio; });
}
bool Hub::held_recipient(uint8_t key,BusSide side,uint8_t address)const{
    return std::any_of(held_keys_.begin(),held_keys_.end(),[&](const auto& held){return held.raw_key==key&&held.bus==side&&held.address==address;});
}
void Hub::tick(uint64_t now_us) {
    now_us_ = now_us;
    if (audio_query_pending_ && now_us_ >= audio_query_due_ && !audio_key_held()) request_audio_status();
}
void Hub::invalidate_endpoint(BusSide side, uint8_t address) {
    transport_.invalidate_binding(side, address);
    ++binding_generation_;
    // Never send cleanup to an address whose appliance may have changed.
    for (auto it = held_keys_.begin(); it != held_keys_.end();) {
        if (it->bus == side && it->address == address) { ++uncertain_releases_; it = held_keys_.erase(it); }
        else ++it;
    }
    audio_query_pending_ = false;
    audio_status_known_ = false;
    for (auto &d : devices_) if (d.bus == side && d.address == address) d.resolved = false;
}
void Hub::clear_devices() { devices_.clear(); discovery_.clear(); discovering_ = false; }
void Hub::begin_discovery() { discovery_.clear(); discovered_physical_ = 0xffff; discovering_ = true; }
void Hub::determine_front_physical_address(uint16_t fallback) {
    if (physical_ != 0xffff) return;
    // A parent precedes its descendants numerically (2.0.0.0 < 2.1.0.0).
    // Keep the chosen address, including fallback, for the rest of this boot.
    physical_ = discovered_physical_ != 0xffff ? discovered_physical_ : fallback;
}
void Hub::finish_discovery() {
    if (!discovering_) return;
    const auto *before = known(current().source);
    const bool had_source = before != nullptr;
    const Device previous = before ? *before : Device{};
    devices_ = std::move(discovery_);
    discovering_ = false;
    const auto *after = known(current().source);
    if (after && after->physical_address != 0xffff && (!had_source || previous.bus != after->bus ||
        previous.address != after->address || previous.physical_address != after->physical_address)) input(current());
}
Device *Hub::discovery_device(BusSide side, uint8_t address) {
    auto &devices = discovering_ ? discovery_ : devices_;
    for (auto &d : devices) if (d.bus == side && d.address == address) return &d;
    return nullptr;
}
void Hub::resolve_device(BusSide side, uint8_t address, bool restore_route) {
    auto *found = discovery_device(side, address);
    if (!found) return;
    found->resolved = !found->name.empty();
    const auto device = *found;
    auto &devices = discovering_ ? discovery_ : devices_;
    // Python's dictionary replaces the previous record when a known OSD name
    // moves to a new logical address. Physical-address conflicts are bus local.
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Device &other) {
        if (other.bus == side && other.address == address) return false;
        return (!device.name.empty() && other.name == device.name && (!qualified_names_ || other.bus == side)) ||
               (device.physical_address != 0xffff && other.bus == side && other.physical_address == device.physical_address);
    }), devices.end());
    if (!discovering_ && restore_route && device.name == current().source &&
        !device.name.empty() && device.physical_address != 0xffff) input(current());
}
void Hub::discovered(BusSide side, uint8_t address) {
    if (address > 14 || discovery_device(side, address)) return;
    Device d; d.bus = side; d.address = address;
    if (address == 0) { d.name = "TV"; d.resolved = true; d.physical_address = 0; d.type = 0; }
    (discovering_ ? discovery_ : devices_).push_back(d);
    send(side, address, {0x83});
    send(side, address, {0x46});
    send(side, address, {0x8c});
}
void Hub::source_thief(const cec::Frame &frame) {
    if (selected_ < 0) { force_standby(); return; }
    const auto a = current();
    bool fix = a.source.empty();
    if (auto *d = find(a.source)) fix = d->address != frame.source();
    if (fix) { send(BusSide::Back, frame.source(), {0x36}); input(a); }
}
void Hub::identity(BusSide side, const cec::Frame &frame) {
    const auto src = frame.source(), op = frame.data[1];
    const bool front = side == BusSide::Front;
    const uint16_t pa = front ? physical_ : 0;
    if (frame.destination() == 15) return;
    switch (op) {
    case 0x83:
        if (pa != 0xffff) send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)});
        break;
    case 0x8c: send(side, 15, {0x87, 0, 0, 0}); break;
    case 0xa5: send(side, 15, {0xa6, 6, static_cast<uint8_t>(front ? 0x10 : 0x80), 0, 0}); break;
    case 0x46: if (src < 15) send(side, src, {0x47, 'a', 'm', 'i', 't', 'y'}); break;
    case 0x9f: if (src < 15) send(side, src, {0x9e, 6}); break;
    case 0xff: if (src < 15) send(side, src, {0, 0xff, 4}); break;
    case 0x8f: {
        if (src == 15) break;
        bool on = selected_ >= 0;
        if (on && !front) { auto *d = find(current().source); on = d && d->bus == side && d->address == src; }
        send(side, src, {0x90, static_cast<uint8_t>(on ? 0 : 1)});
        break;
    }
    default: break; // Python is a CEC follower: unspecified opcodes are ignored.
    }
}
void Hub::receive(BusSide side, const cec::Frame &frame) {
    if (frame.size < 2 || frame.size > 16 || frame.source() == (side == BusSide::Front ? front_ : back_)) return;
    if (frame.destination() != 15 && frame.destination() != (side == BusSide::Front ? front_ : back_)) return;
    const auto op = frame.data[1];
    // The Linux CEC core enforces minimum lengths, not exact lengths.
    identity(side, frame);
    if (op == 0x84 && frame.size >= 5 && frame.destination() == 15 && frame.source() < 15) {
        discovered(side, frame.source());
        auto *d = discovery_device(side, frame.source());
        const auto physical = static_cast<uint16_t>((frame.data[2] << 8) | frame.data[3]);
        d->physical_address = physical;
        // Only actual reports from the initial back scan count; a poll ACK or
        // a synthesized TV record is not physical-address evidence. Collect
        // before name/address deduplication can remove a responding device.
        if (discovering_ && physical_ == 0xffff && side == BusSide::Back &&
            usable_front_physical_address(d->physical_address))
            discovered_physical_ = std::min(discovered_physical_, d->physical_address);
        d->type = frame.data[4];
        resolve_device(side, frame.source(), true);
    } else if (op == 0x47 && frame.destination() != 15) {
        if (auto *d = discovery_device(side, frame.source())) {
            const auto old_name = d->name;
            d->name.clear();
            for (uint8_t i = 2; i < frame.size; ++i) if (frame.data[i]) d->name += static_cast<char>(frame.data[i]);
            const auto reported = d->name;
            if (!old_name.empty() && old_name != reported) invalidate_endpoint(side,frame.source());
            resolve_device(side, frame.source(), old_name != reported);
        }
    } else if (op == 0x87 && frame.size >= 5 && frame.destination() == 15) {
        if (auto *d = discovery_device(side, frame.source())) d->vendor_id = (frame.data[2] << 16) | (frame.data[3] << 8) | frame.data[4];
    } else if (op == 0x7a && frame.size >= 3 && frame.destination() != 15) {
        auto* d = owner_audio_side_ ? find_on_side(*owner_audio_side_,current().audio) : find(current().audio);
        if (d && d->bus == side && d->address == frame.source()) {
            muted_ = (frame.data[2] & 0x80) != 0;
            const auto volume = frame.data[2] & 0x7f;
            volume_ = volume <= 100 ? volume : 127;
            audio_status_known_ = true;
        }
    }
    if (side == BusSide::Back && ((op == 0x04 && frame.destination() != 15) ||
        (op == 0x82 && frame.size >= 4 && frame.destination() == 15))) source_thief(frame);
}
}
