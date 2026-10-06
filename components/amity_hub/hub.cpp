#include "amity/hub.hpp"
#include <algorithm>
#include <utility>

namespace amity {
namespace {
std::vector<std::string> names(const Activity &a) {
    return {a.display, a.source, a.audio, a.switch_device};
}
}
void Hub::configure(std::vector<Activity> activities, std::vector<Quirk> quirks) {
    if (selected_ >= 0) standby();
    ++activity_generation_;
    activities_ = std::move(activities);
    quirks_ = std::move(quirks);
}
void Hub::set_addresses(uint8_t front, uint8_t back, uint16_t physical) {
    front_ = front; back_ = back; physical_ = physical;
}
void Hub::announce(BusSide side) {
    const bool front = side == BusSide::Front;
    if ((front ? front_ : back_) == 15) return;
    const uint16_t pa = front ? physical_ : 0;
    // Linux announces features first for a CEC 2.0 adapter, then its physical
    // address and vendor. Preserve both payloads and ordering.
    send(side, 15, {0xa6, 6, static_cast<uint8_t>(front ? 0x10 : 0x80), 0, 0});
    send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)});
    send(side, 15, {0x87, 0, 0, 0});
}
Snapshot Hub::snapshot() const {
    Snapshot s;
    s.activities = activities_; s.devices = devices_; s.current_activity = selected_;
    s.activity_generation = activity_generation_;
    s.front_address = front_; s.back_address = back_; s.ready = front_ < 15 && back_ < 15;
    s.audio_status_known = audio_status_known_; s.muted = muted_; s.volume = volume_;
    return s;
}
Activity Hub::current() const {
    return selected_ >= 0 && static_cast<size_t>(selected_) < activities_.size() ? activities_[selected_] : Activity{};
}
Device *Hub::find(const std::string &name) {
    if (name.empty()) return nullptr;
    for (auto &d : devices_) if (d.name == name) return &d;
    transport_.request_scan();
    return nullptr;
}
const Device *Hub::known(const std::string &name) const {
    if (!name.empty()) for (const auto &d : devices_) if (d.name == name) return &d;
    return nullptr;
}
bool Hub::send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data) {
    const bool sent = transport_.send(side, dest, data.begin(), data.size());
    // Power quirks and switch selection also produce press/release pairs.
    // Preserve a failed release independently of the user-held key state.
    if (dest < 15 && data.size() && *data.begin() == 0x45) {
        auto &pending = pending_releases_[side == BusSide::Front ? 0 : 1];
        if (sent) pending &= ~(1u << dest);
        else pending |= 1u << dest;
    }
    return sent;
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
void Hub::power(const Device &device, bool on, bool quirk) {
    if (quirk) for (const auto &q : quirks_) {
        if (q.vendor_id != device.vendor_id) continue;
        const auto &data = on ? q.power_on : q.power_off;
        if (data.empty() || data.size() > 15) continue;
        transport_.send(device.bus, device.address, data.data(), data.size());
        if (data[0] == 0x44) send(device.bus, device.address, {0x45});
        return;
    }
    if (!on) send(device.bus, device.address, {0x36});
    else if (device.type == 0) send(device.bus, device.address, {0x04});
    else {
        send(device.bus, device.address, {0x44, 0x6d});
        send(device.bus, device.address, {0x45});
    }
}
void Hub::active_source() {
    send(BusSide::Front, 15, {0x82, static_cast<uint8_t>(physical_ >> 8), static_cast<uint8_t>(physical_)});
}
void Hub::input(const Activity &activity) {
    if (auto *d = find(activity.source)) {
        send(d->bus, 15, {0x86, static_cast<uint8_t>(d->physical_address >> 8), static_cast<uint8_t>(d->physical_address)});
        return;
    }
    if (!activity.switch_device.empty()) {
        auto *d = find(activity.switch_device);
        if (!d) return;
        send(d->bus, d->address, {0x44, 0x69, activity.switch_input});
        send(d->bus, d->address, {0x45});
    }
    active_source();
}
bool Hub::select_activity(int index) {
    if (index < -1 || index >= static_cast<int>(activities_.size())) return false;
    if (index == selected_) { fix_current_activity(); return true; }
    release_key();
    const auto before = names(current());
    const auto after_activity = index < 0 ? Activity{} : activities_[index];
    const auto after = names(after_activity);
    for (const auto &name : before) if (std::find(after.begin(), after.end(), name) == after.end()) {
        if (auto *d = find(name)) power(*d, false, index == -1);
    }
    for (const auto &name : after) if (std::find(before.begin(), before.end(), name) == before.end()) {
        if (auto *d = find(name)) power(*d, true);
    }
    input(after_activity);
    selected_ = index;
    ++activity_generation_;
    audio_status_known_ = false; volume_ = 127;
    request_audio_status();
    if (index < 0) playing_ = false;
    return true;
}
void Hub::fix_current_activity() {
    const auto a = current();
    for (const auto &name : names(a)) if (auto *d = find(name)) power(*d, true);
    input(a);
    request_audio_status();
}
void Hub::request_audio_status() {
    if (const auto *d = known(current().audio)) {
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
    send(BusSide::Front, 15, {0x36});
    send(BusSide::Back, 15, {0x36});
}
bool Hub::press_key(uint8_t key, bool repeat) {
    const auto a = current();
    if (key == 0x40) { standby(); return true; }
    if (release_pending_ && !release_key()) return false;
    const uint8_t raw_key = key;
    // Default Python hub behavior emulates the play/pause toggle.
    if (key == 0x61) {
        if (key_held_ && held_raw_key_ == raw_key) key = held_key_;
        else key = playing_ ? 0x46 : 0x44;
    }
    const bool audio_key = (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66;
    auto *d = find(audio_key ? a.audio : a.source);
    if (!d) return false;
    if (key_held_ && (held_bus_ != d->bus || held_address_ != d->address) && !release_key()) return false;
    if ((pending_releases_[d->bus == BusSide::Front ? 0 : 1] & (1u << d->address)) &&
        !send(d->bus, d->address, {0x45})) return false;
    if (!send(d->bus, d->address, {0x44, key})) return false;
    if (raw_key == 0x61 && !(key_held_ && held_raw_key_ == raw_key)) playing_ = !playing_;
    held_bus_ = d->bus; held_address_ = d->address; key_held_ = true;
    held_raw_key_ = raw_key; held_key_ = key;
    held_audio_ = audio_key;
    if (audio_key) audio_status_known_ = false;
    if (!repeat) release_key();
    return true;
}
bool Hub::release_key() {
    if (!key_held_) return true;
    if (!send(held_bus_, held_address_, {0x45})) { release_pending_ = true; return false; }
    key_held_ = false;
    release_pending_ = false;
    if (held_audio_) {
        audio_query_pending_ = true;
        audio_query_due_ = now_us_ + 500000;
    }
    held_audio_ = false;
    return true;
}
void Hub::tick(uint64_t now_us) {
    now_us_ = now_us;
    if (release_pending_) release_key();
    for (const auto side : {BusSide::Front, BusSide::Back}) {
        const auto pending = pending_releases_[side == BusSide::Front ? 0 : 1];
        for (uint8_t address = 0; address < 15; ++address) {
            if (!(pending & (1u << address))) continue;
            // The held-key path retries and updates its state itself.
            if (release_pending_ && held_bus_ == side && held_address_ == address) continue;
            send(side, address, {0x45});
        }
    }
    if (audio_query_pending_ && now_us_ >= audio_query_due_ && !held_audio_) request_audio_status();
}
void Hub::clear_devices() { devices_.clear(); discovery_.clear(); discovering_ = false; }
void Hub::begin_discovery() { discovery_.clear(); discovering_ = true; }
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
    const auto device = *found;
    auto &devices = discovering_ ? discovery_ : devices_;
    // Python's dictionary replaces the previous record when a known OSD name
    // moves to a new logical address. Physical-address conflicts are bus local.
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Device &other) {
        if (other.bus == side && other.address == address) return false;
        return (!device.name.empty() && other.name == device.name) ||
               (device.physical_address != 0xffff && other.bus == side && other.physical_address == device.physical_address);
    }), devices.end());
    if (!discovering_ && restore_route && device.name == current().source &&
        !device.name.empty() && device.physical_address != 0xffff) input(current());
}
void Hub::discovered(BusSide side, uint8_t address) {
    if (address > 14 || discovery_device(side, address)) return;
    Device d; d.bus = side; d.address = address;
    if (address == 0) { d.name = "TV"; d.physical_address = 0; d.type = 0; }
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
    case 0x83: send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)}); break;
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
        d->physical_address = static_cast<uint16_t>((frame.data[2] << 8) | frame.data[3]);
        d->type = frame.data[4];
        resolve_device(side, frame.source(), true);
    } else if (op == 0x47 && frame.destination() != 15) {
        if (auto *d = discovery_device(side, frame.source())) {
            const auto old_name = d->name;
            d->name.clear();
            for (uint8_t i = 2; i < frame.size; ++i) if (frame.data[i]) d->name += static_cast<char>(frame.data[i]);
            resolve_device(side, frame.source(), old_name != d->name);
        }
    } else if (op == 0x87 && frame.size >= 5 && frame.destination() == 15) {
        if (auto *d = discovery_device(side, frame.source())) d->vendor_id = (frame.data[2] << 16) | (frame.data[3] << 8) | frame.data[4];
    } else if (op == 0x7a && frame.size >= 3 && frame.destination() != 15) {
        if (auto *d = find(current().audio); d && d->bus == side && d->address == frame.source()) {
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
