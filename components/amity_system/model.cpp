#include "amity/system/model.hpp"

#include <limits>
#include <map>

namespace amity::system {

std::optional<uint64_t> parse_uint64(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text[0] == '0')) return {};
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return {};
        auto digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return {};
        value = value * 10 + digit;
    }
    return value;
}
std::string format_uint64(uint64_t value) { return std::to_string(value); }
std::optional<Side> parse_side(std::string_view text) {
    if (text == "front") return Side::front;
    if (text == "back") return Side::back;
    return {};
}
const char* side_name(Side side) { return side == Side::front ? "front" : "back"; }
bool CecPortRef::valid() const {
    return consortium_id.valid() && octopus_id.valid() && tentacle_id <= amity::last_tentacle_id &&
           (side == Side::front || side == Side::back);
}
bool QualifiedDeviceName::valid() const {
    return port.valid() && !osd_name.empty() && osd_name.size() <= 14 &&
           std::none_of(osd_name.begin(), osd_name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string cec_uri(const FullyQualifiedCecAddress& a) {
    if (!a.valid()) return {};
    return "cec://" + a.port.consortium_id.str() + "/" + a.port.octopus_id.str() + "/" +
           std::to_string(a.port.tentacle_id) + "/" + side_name(a.port.side) + "/" + std::to_string(a.logical_address);
}
std::optional<FullyQualifiedCecAddress> parse_cec_uri(std::string_view text) {
    if (text.size() > 82 || text.substr(0, 6) != "cec://") return {};
    text.remove_prefix(6);
    std::array<std::string_view, 5> fields{};
    for (size_t i = 0; i < fields.size(); ++i) {
        size_t slash = text.find('/');
        if ((i == 4) != (slash == std::string_view::npos)) return {};
        fields[i] = text.substr(0, slash);
        if (slash != std::string_view::npos) text.remove_prefix(slash + 1);
    }
    auto consortium = ConsortiumId::parse(fields[0]); auto octopus = OctopusId::parse(fields[1]);
    auto tentacle = parse_uint64(fields[2]); auto side = parse_side(fields[3]); auto la = parse_uint64(fields[4]);
    if (!consortium || !octopus || !tentacle || *tentacle > amity::last_tentacle_id || !side || !la || *la > 14) return {};
    return FullyQualifiedCecAddress{{*consortium, *octopus, static_cast<uint8_t>(*tentacle), *side}, static_cast<uint8_t>(*la)};
}
namespace {
std::string quote(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}
struct JsonValue { std::string text; bool quoted = false; };
// Small strict decoder for the two flat edge objects. Duplicate/unknown fields,
// number coercions and trailing data are never accepted.
class ObjectReader {
public:
    explicit ObjectReader(std::string_view input) : input_(input) {}
    std::optional<std::map<std::string, JsonValue>> read() {
        std::map<std::string, JsonValue> out;
        if (!take('{')) return {};
        if (take('}')) { skip(); return input_.empty() ? std::optional(out) : std::nullopt; }
        do {
            auto key = string(); if (!key || !take(':')) return {};
            JsonValue value;
            skip();
            if (!input_.empty() && input_[0] == '"') {
                auto decoded = string(); if (!decoded) return {};
                value = {*decoded, true};
            } else {
                size_t count = 0;
                while (count < input_.size() && input_[count] >= '0' && input_[count] <= '9') ++count;
                if (!count) return {};
                value.text = std::string(input_.substr(0, count)); input_.remove_prefix(count);
            }
            if (!out.emplace(*key, value).second) return {};
        } while (take(','));
        if (!take('}')) return {};
        skip(); if (!input_.empty()) return {};
        return out;
    }
private:
    void skip() { while (!input_.empty() && (input_[0] == ' ' || input_[0] == '\n' || input_[0] == '\r' || input_[0] == '\t')) input_.remove_prefix(1); }
    bool take(char c) { skip(); if (input_.empty() || input_[0] != c) return false; input_.remove_prefix(1); return true; }
    std::optional<std::string> string() {
        if (!take('"')) return {};
        std::string out;
        while (!input_.empty()) {
            unsigned char c = input_[0]; input_.remove_prefix(1);
            if (c == '"') return out;
            if (c < 32) return {};
            if (c == '\\') {
                if (input_.empty()) return {};
                c = input_[0]; input_.remove_prefix(1);
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                default: return {}; // CEC's OSD byte decoder, not escaped Unicode, defines names.
                }
            }
            out += static_cast<char>(c);
        }
        return {};
    }
    std::string_view input_;
};
std::string port_json_prefix(const CecPortRef& p) {
    return "{\"consortium_id\":" + quote(p.consortium_id.str()) + ",\"octopus_id\":" + quote(p.octopus_id.str()) +
           ",\"tentacle_id\":" + std::to_string(p.tentacle_id) + ",\"side\":" + quote(side_name(p.side));
}
std::optional<CecPortRef> decode_port(const std::map<std::string, JsonValue>& fields) {
    for (const auto* key : {"consortium_id", "octopus_id", "side"}) {
        auto it = fields.find(key); if (it == fields.end() || !it->second.quoted) return {};
    }
    auto ti = fields.find("tentacle_id"); if (ti == fields.end() || ti->second.quoted) return {};
    auto cid = ConsortiumId::parse(fields.at("consortium_id").text); auto oid = OctopusId::parse(fields.at("octopus_id").text);
    auto side = parse_side(fields.at("side").text); auto tentacle = parse_uint64(ti->second.text);
    if (!cid || !oid || !side || !tentacle || *tentacle > amity::last_tentacle_id) return {};
    return CecPortRef{*cid, *oid, static_cast<uint8_t>(*tentacle), *side};
}
}
std::string name_json(const QualifiedDeviceName& n) {
    return n.valid() ? port_json_prefix(n.port) + ",\"osd_name\":" + quote(n.osd_name) + "}" : std::string{};
}
std::string address_json(const FullyQualifiedCecAddress& a) {
    return a.valid() ? port_json_prefix(a.port) + ",\"logical_address\":" + std::to_string(a.logical_address) + "}" : std::string{};
}
std::optional<QualifiedDeviceName> parse_name_json(std::string_view text) {
    if (text.size() > 512) return {};
    auto fields = ObjectReader(text).read(); if (!fields || fields->size() != 5) return {};
    auto port = decode_port(*fields); auto name = fields->find("osd_name");
    if (!port || name == fields->end() || !name->second.quoted) return {};
    QualifiedDeviceName n{*port, name->second.text}; return n.valid() ? std::optional(n) : std::nullopt;
}
std::optional<FullyQualifiedCecAddress> parse_address_json(std::string_view text) {
    if (text.size() > 512) return {};
    auto fields = ObjectReader(text).read(); if (!fields || fields->size() != 5) return {};
    auto port = decode_port(*fields); auto address = fields->find("logical_address");
    if (!port || address == fields->end() || address->second.quoted) return {};
    auto la = parse_uint64(address->second.text); if (!la || *la > 14) return {};
    return FullyQualifiedCecAddress{*port, static_cast<uint8_t>(*la)};
}
bool valid_physical_address(uint16_t address, bool allow_root) {
    if (address == 0xffff || (!allow_root && address == 0)) return false;
    bool zero = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        uint8_t part = (address >> shift) & 15;
        if (zero && part != 0) return false;
        if (part == 0) zero = true;
    }
    return true;
}
} // namespace amity::system
