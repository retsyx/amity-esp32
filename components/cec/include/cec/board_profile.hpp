/* Copyright 2026. GPL-3.0-or-later. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace cec {
enum class Side : uint8_t { Front, Back };
enum class BoardProfileId : uint8_t { LegacyBoard2 = 1, LegacyButtonless = 2, Development5Tentacle = 3 };
struct PortPin {
    uint8_t tentacle_id = 0;
    Side side = Side::Front;
    int gpio = -1;
};
struct PresencePin {
    int gpio = -1;
    bool present_level = true;
    bool pullup = false;
    bool pulldown = false;
};
struct BoardProfile {
    BoardProfileId id;
    uint32_t revision;
    uint8_t tentacle_count;
    PortPin ports[10];
    int reset_button = -1;
    int status_led = -1;
    PresencePin presence[5]{};
};
// These mappings describe the existing board. They do not qualify additional
// tentacles or new setup gestures, indicators, or HDMI electrical performance.
inline constexpr BoardProfile legacy_board2_profile{
    BoardProfileId::LegacyBoard2, 1, 1, {{0, Side::Front, 1}, {0, Side::Back, 2}}, 4, -1};
inline constexpr BoardProfile legacy_buttonless_profile{
    BoardProfileId::LegacyButtonless, 1, 1, {{0, Side::Front, 1}, {0, Side::Back, 2}}, -1, -1};

enum class ProfileError : uint8_t {
    None, InvalidCount, InvalidConnector, InvalidGpio, DuplicateGpio, PeripheralConflict, Unsupported
};
inline bool connector_enabled(const BoardProfile& profile, uint8_t id) {
    if (id >= profile.tentacle_count) return false;
    bool front = false, back = false;
    for (size_t i = 0; i < 2u * profile.tentacle_count; ++i) if (profile.ports[i].tentacle_id == id) {
        if (profile.ports[i].side == Side::Front) front = profile.ports[i].gpio >= 0;
        if (profile.ports[i].side == Side::Back) back = profile.ports[i].gpio >= 0;
    }
    return front && back;
}
// Target validity comes from GPIO_IS_VALID_OUTPUT_GPIO in firmware. Reserved
// pins are enabled peripherals, not a speculative portable GPIO assignment.
inline ProfileError validate_board_profile(const BoardProfile &profile,
        bool (*valid_output)(int), const int *reserved = nullptr, size_t reserved_count = 0,
        bool (*valid_input)(int) = nullptr) {
    if (!profile.tentacle_count || profile.tentacle_count > 5) return ProfileError::InvalidCount;
    const bool development = profile.id == BoardProfileId::Development5Tentacle;
    if (development && (profile.revision != 1 || profile.tentacle_count != 5)) return ProfileError::Unsupported;
    bool connectors[10]{};
    const size_t count = 2u * profile.tentacle_count;
    for (size_t i = 0; i < count; ++i) {
        const auto &port = profile.ports[i];
        if (port.tentacle_id >= profile.tentacle_count ||
            (port.side != Side::Front && port.side != Side::Back)) return ProfileError::InvalidConnector;
        const size_t connector = 2u * port.tentacle_id + (port.side == Side::Back ? 1u : 0u);
        if (connectors[connector]) return ProfileError::InvalidConnector;
        connectors[connector] = true;
        if (development && port.gpio == -1) continue;
        if (!valid_output || !valid_output(port.gpio)) return ProfileError::InvalidGpio;
        for (size_t j = 0; j < i; ++j)
            if (profile.ports[j].gpio == port.gpio) return ProfileError::DuplicateGpio;
        if (port.gpio == profile.reset_button || port.gpio == profile.status_led)
            return ProfileError::PeripheralConflict;
        for (size_t j = 0; j < reserved_count; ++j)
            if (port.gpio == reserved[j]) return ProfileError::PeripheralConflict;
    }
    for (uint8_t id = 0; id < profile.tentacle_count; ++id) {
        bool front = false, back = false;
        for (size_t i = 0; i < count; ++i) if (profile.ports[i].tentacle_id == id) {
            if (profile.ports[i].side == Side::Front) front = profile.ports[i].gpio >= 0;
            if (profile.ports[i].side == Side::Back) back = profile.ports[i].gpio >= 0;
        }
        if (front != back) return ProfileError::InvalidConnector;
        const auto& presence = profile.presence[id];
        if (presence.gpio == -1) continue;
        if (!development || !front || presence.gpio < 0 || !valid_input || !valid_input(presence.gpio) || (presence.pullup && presence.pulldown)) return ProfileError::InvalidGpio;
        if (presence.gpio == profile.reset_button || presence.gpio == profile.status_led) return ProfileError::PeripheralConflict;
        for (size_t j = 0; j < count; ++j) if (profile.ports[j].gpio == presence.gpio) return ProfileError::DuplicateGpio;
        for (uint8_t j = 0; j < id; ++j) if (profile.presence[j].gpio == presence.gpio) return ProfileError::DuplicateGpio;
        for (size_t j = 0; j < reserved_count; ++j) if (reserved[j] == presence.gpio) return ProfileError::PeripheralConflict;
    }
    if (profile.reset_button >= 0 && profile.reset_button == profile.status_led)
        return ProfileError::PeripheralConflict;
    for (size_t j = 0; j < reserved_count; ++j)
        if ((profile.reset_button >= 0 && profile.reset_button == reserved[j]) ||
            (profile.status_led >= 0 && profile.status_led == reserved[j])) return ProfileError::PeripheralConflict;
    if (development) {
        if ((profile.reset_button >= 0 && (!valid_input || !valid_input(profile.reset_button))) ||
            (profile.status_led >= 0 && (!valid_output || !valid_output(profile.status_led)))) return ProfileError::InvalidGpio;
        return ProfileError::None;
    }
    const BoardProfile *supported = nullptr;
    if (profile.id == BoardProfileId::LegacyBoard2) supported = &legacy_board2_profile;
    if (profile.id == BoardProfileId::LegacyButtonless) supported = &legacy_buttonless_profile;
    if (!supported || profile.revision != supported->revision ||
        profile.tentacle_count != supported->tentacle_count ||
        profile.reset_button != supported->reset_button || profile.status_led != supported->status_led)
        return ProfileError::Unsupported;
    for (size_t i = 0; i < count; ++i) {
        const auto &port = profile.ports[i];
        const auto &expected = supported->ports[2u * port.tentacle_id + (port.side == Side::Back ? 1u : 0u)];
        if (port.gpio != expected.gpio) return ProfileError::Unsupported;
    }
    return ProfileError::None;
}
}
