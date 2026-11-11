#pragma once

#include "amity/hardware_limits.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amity::system {

// Only edge codecs handle text. Different tags cannot be accidentally assigned.
template <class Tag> class Id {
public:
    static std::optional<Id> parse(std::string_view text) {
        if (text.size() != 32) return {};
        Id value;
        for (size_t i = 0; i < 16; ++i) {
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int a = nibble(text[i * 2]), b = nibble(text[i * 2 + 1]);
            if (a < 0 || b < 0) return {};
            value.bytes_[i] = static_cast<uint8_t>((a << 4) | b);
        }
        if (!value.valid()) return {};
        return value;
    }
    static std::optional<Id> from_bytes(const std::array<uint8_t, 16>& bytes) {
        Id value;
        value.bytes_ = bytes;
        if (!value.valid()) return {};
        return value;
    }
    // The origin/storage adapter must supply a cryptographically secure RNG.
    static std::optional<Id> generate(const std::function<bool(uint8_t*, size_t)>& secure_random) {
        Id value;
        if (!secure_random(value.bytes_.data(), value.bytes_.size()) || !value.valid()) return {};
        return value;
    }
    bool valid() const { return std::any_of(bytes_.begin(), bytes_.end(), [](uint8_t b) { return b != 0; }); }
    explicit operator bool() const { return valid(); }
    std::string str() const {
        static constexpr char hex[] = "0123456789abcdef";
        std::string result(32, '0');
        for (size_t i = 0; i < 16; ++i) {
            result[i * 2] = hex[bytes_[i] >> 4]; result[i * 2 + 1] = hex[bytes_[i] & 15];
        }
        return result;
    }
    std::string to_string() const { return str(); }
    const std::array<uint8_t, 16>& bytes() const { return bytes_; }
    friend bool operator==(const Id& a, const Id& b) { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const Id& a, const Id& b) { return !(a == b); }
    friend bool operator<(const Id& a, const Id& b) { return a.bytes_ < b.bytes_; }
private:
    std::array<uint8_t, 16> bytes_{};
};

#define AMITY_SYSTEM_ID(Name) struct Name##Tag {}; using Name = Id<Name##Tag>
AMITY_SYSTEM_ID(OctopusId); AMITY_SYSTEM_ID(ConsortiumId); AMITY_SYSTEM_ID(BootId);
AMITY_SYSTEM_ID(ComponentId);
AMITY_SYSTEM_ID(ActivityId); AMITY_SYSTEM_ID(ZoneId);
AMITY_SYSTEM_ID(SessionId);
AMITY_SYSTEM_ID(MessageId); AMITY_SYSTEM_ID(ScanId);
AMITY_SYSTEM_ID(LinkId);
AMITY_SYSTEM_ID(JoinId); AMITY_SYSTEM_ID(MutationId);
#undef AMITY_SYSTEM_ID

std::optional<uint64_t> parse_uint64(std::string_view);
std::string format_uint64(uint64_t);
enum class Side { front, back };
std::optional<Side> parse_side(std::string_view);
const char* side_name(Side);

struct CecPortRef {
    ConsortiumId consortium_id;
    OctopusId octopus_id;
    uint8_t tentacle_id = 0;
    Side side = Side::front;
    bool valid() const;
    friend bool operator==(const CecPortRef& a, const CecPortRef& b) {
        return a.consortium_id == b.consortium_id && a.octopus_id == b.octopus_id &&
               a.tentacle_id == b.tentacle_id && a.side == b.side;
    }
    friend bool operator!=(const CecPortRef& a, const CecPortRef& b) { return !(a == b); }
};
struct QualifiedDeviceName {
    CecPortRef port;
    std::string osd_name;
    bool valid() const;
    friend bool operator==(const QualifiedDeviceName& a, const QualifiedDeviceName& b) {
        return a.port == b.port && a.osd_name == b.osd_name;
    }
    friend bool operator!=(const QualifiedDeviceName& a, const QualifiedDeviceName& b) { return !(a == b); }
};
struct FullyQualifiedCecAddress {
    CecPortRef port;
    uint8_t logical_address = 15; // Invalid until resolved; never a persisted selector.
    bool valid() const { return port.valid() && logical_address < 15; }
    friend bool operator==(const FullyQualifiedCecAddress& a, const FullyQualifiedCecAddress& b) {
        return a.port == b.port && a.logical_address == b.logical_address;
    }
    friend bool operator!=(const FullyQualifiedCecAddress& a, const FullyQualifiedCecAddress& b) { return !(a == b); }
};
std::string cec_uri(const FullyQualifiedCecAddress&);
std::optional<FullyQualifiedCecAddress> parse_cec_uri(std::string_view);
std::string name_json(const QualifiedDeviceName&);
std::string address_json(const FullyQualifiedCecAddress&);
std::optional<QualifiedDeviceName> parse_name_json(std::string_view);
std::optional<FullyQualifiedCecAddress> parse_address_json(std::string_view);
bool valid_physical_address(uint16_t, bool allow_root = true);

struct Component {
    ComponentId component_id;
    std::string label;
    std::vector<QualifiedDeviceName> qualified_names;
};
} // namespace amity::system
