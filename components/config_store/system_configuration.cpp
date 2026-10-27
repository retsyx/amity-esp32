#include "system_configuration.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#ifdef ESP_PLATFORM
#include "psa/crypto.h"
#else
#include <openssl/sha.h>
#endif

namespace config_store {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
template<class T> bool has_config(const std::set<T>& values,const T& id){return values.count(id)!=0;}
template<class T,class F> void retain_config(std::vector<T>& values,F predicate){values.erase(std::remove_if(values.begin(),values.end(),[&](const auto& v){return !predicate(v);}),values.end());}
bool keys(cJSON* v, std::initializer_list<const char*> allowed) {
    if (!cJSON_IsObject(v)) return false;
    std::set<std::string> seen;
    for (auto* c = v->child; c; c = c->next) {
        if (!c->string || !seen.insert(c->string).second) return false;
        bool known = false; for (auto* k : allowed) if (!std::strcmp(c->string, k)) known = true;
        if (!known) return false;
    }
    return true;
}
bool unique(cJSON* v) {
    if (cJSON_IsObject(v)) {
        std::set<std::string> seen;
        for (auto* c = v->child; c; c = c->next) if (!c->string || !seen.insert(c->string).second || !unique(c)) return false;
    } else if (cJSON_IsArray(v)) for (auto* c = v->child; c; c = c->next) if (!unique(c)) return false;
    return true;
}
Json parse(const std::string& text) {
    if (text.size() > configuration_compatibility_limit || text.find('\0') != std::string::npos) return {nullptr, cJSON_Delete};
    unsigned depth = 0; bool quoted = false, escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) { if (c == 'u' && text.compare(i + 1, 4, "0000") == 0) return {nullptr, cJSON_Delete}; escaped = false; }
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 8) return {nullptr, cJSON_Delete}; }
        else if (c == ']' || c == '}') { if (!depth) return {nullptr, cJSON_Delete}; --depth; }
    }
    Json root(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true), cJSON_Delete);
    if (root && !unique(root.get())) root.reset();
    return root;
}
cJSON* field(cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
bool text(cJSON* o, const char* k, std::string& out, size_t limit = 128, bool optional = false) {
    auto* v = field(o, k); if (!v && optional) { out.clear(); return true; }
    if (!cJSON_IsString(v) || !v->valuestring || std::strlen(v->valuestring) > limit) return false;
    out = v->valuestring;
    for (unsigned char c : out) if (c < 32 || c == 127) return false;
    return true;
}
bool boolean(cJSON* o, const char* k, bool& out, bool optional = false) {
    auto* v = field(o, k); if (!v && optional) { out = false; return true; }
    if (!cJSON_IsBool(v)) return false;
    out = cJSON_IsTrue(v); return true;
}
bool number(cJSON* o, const char* k, uint64_t& out, uint64_t maximum) {
    auto* v = field(o, k);
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > static_cast<double>(maximum) || std::floor(v->valuedouble) != v->valuedouble) return false;
    out = static_cast<uint64_t>(v->valuedouble); return true;
}
bool revision(cJSON* o, const char* k, uint64_t& out) {
    std::string s; if (!text(o, k, s, 20)) return false;
    auto parsed = model::parse_uint64(s); if (!parsed) return false; out = *parsed; return true;
}
template<class T> bool identifier(cJSON* o, const char* k, T& out) {
    std::string s; if (!text(o, k, s, 32)) return false;
    auto parsed = T::parse(s); if (!parsed) return false; out = *parsed; return true;
}
template<class T> bool optional_id(cJSON* o, const char* k, std::optional<T>& out) {
    auto* v = field(o, k); if (!v || cJSON_IsNull(v)) { out.reset(); return true; }
    T id; if (!identifier(o, k, id)) return false; out = id; return true;
}
template<class T, class F> bool list(cJSON* o, const char* k, std::vector<T>& out, size_t maximum, F decoder) {
    auto* a = field(o, k); if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > static_cast<int>(maximum)) return false;
    out.clear(); for (auto* item = a->child; item; item = item->next) { T value; if (!decoder(item, value)) return false; out.push_back(std::move(value)); }
    return true;
}
template<class E> bool enumeration(cJSON* o, const char* k, E& out, std::initializer_list<std::pair<const char*, E>> values) {
    std::string s; if (!text(o, k, s, 32)) return false;
    for (const auto& value : values) if (s == value.first) { out = value.second; return true; }
    return false;
}
const char* capability_name(model::Capability value) {
    switch (value) {
    case model::Capability::power: return "power"; case model::Capability::routing: return "routing";
    case model::Capability::navigation: return "navigation"; case model::Capability::volume: return "volume";
    case model::Capability::audio_status: return "audio_status"; case model::Capability::proxy_activation: return "proxy_activation";
    case model::Capability::identity: return "identity"; case model::Capability::audio: return "audio";
    case model::Capability::query_state: return "query_state";
    }
    return "invalid";
}
bool capability(cJSON* o, model::Capability& v) {
    return enumeration(o, "capability", v, {{"power", model::Capability::power}, {"routing", model::Capability::routing},
        {"navigation", model::Capability::navigation}, {"volume", model::Capability::volume}, {"audio_status", model::Capability::audio_status},
        {"proxy_activation", model::Capability::proxy_activation}, {"identity", model::Capability::identity},
        {"audio", model::Capability::audio}, {"query_state", model::Capability::query_state}});
}
const char* role_name(model::PortRole v) {
    switch (v) { case model::PortRole::disabled: return "disabled"; case model::PortRole::monitor: return "monitor";
    case model::PortRole::tv_controller: return "tv_controller"; case model::PortRole::playback_proxy: return "playback_proxy"; }
    return "invalid";
}
bool role(cJSON* o, model::PortRole& v) {
    return enumeration(o, "role", v, {{"disabled", model::PortRole::disabled}, {"monitor", model::PortRole::monitor},
        {"tv_controller", model::PortRole::tv_controller}, {"playback_proxy", model::PortRole::playback_proxy}});
}
const char* operation_name(model::OperationKind v) {
    switch (v) { case model::OperationKind::ensure_power: return "ensure_power"; case model::OperationKind::select_path: return "select_path";
    case model::OperationKind::announce_active_source: return "announce_active_source"; case model::OperationKind::press_key: return "press_key";
    case model::OperationKind::release_key: return "release_key"; case model::OperationKind::query_state: return "query_state"; }
    return "invalid";
}
bool port(cJSON* o, model::CecPortRef& p) {
    uint64_t index; std::string side;
    if (!keys(o, {"consortium_id", "octopus_id", "tentacle_id", "side"}) || !identifier(o, "consortium_id", p.consortium_id) ||
        !identifier(o, "octopus_id", p.octopus_id) || !number(o, "tentacle_id", index, amity::last_tentacle_id) || !text(o, "side", side, 5)) return false;
    auto s = model::parse_side(side); if (!s) return false; p.tentacle_id = static_cast<uint8_t>(index); p.side = *s; return true;
}
bool name(cJSON* o, model::QualifiedDeviceName& n) {
    if (!keys(o, {"consortium_id", "octopus_id", "tentacle_id", "side", "osd_name"})) return false;
    // Port and name are distinct canonical flat objects, never an LA locator.
    uint64_t index; std::string side;
    if (!identifier(o, "consortium_id", n.port.consortium_id) || !identifier(o, "octopus_id", n.port.octopus_id) ||
        !number(o, "tentacle_id", index, amity::last_tentacle_id) || !text(o, "side", side, 5) || !text(o, "osd_name", n.osd_name, 14)) return false;
    auto s = model::parse_side(side); if (!s) return false; n.port.side = *s; n.port.tentacle_id = static_cast<uint8_t>(index); return n.valid();
}
void string(cJSON* o, const char* k, const std::string& s) { cJSON_AddStringToObject(o, k, s.c_str()); }
template<class T> void id(cJSON* o, const char* k, T v) { string(o, k, v.str()); }
void rev(cJSON* o, const char* k, uint64_t v) { string(o, k, model::format_uint64(v)); }
cJSON* encode_port(const model::CecPortRef& p) {
    auto* o = cJSON_CreateObject(); id(o, "consortium_id", p.consortium_id); id(o, "octopus_id", p.octopus_id);
    cJSON_AddNumberToObject(o, "tentacle_id", p.tentacle_id); cJSON_AddStringToObject(o, "side", model::side_name(p.side)); return o;
}
cJSON* encode_name(const model::QualifiedDeviceName& n) { auto* o = encode_port(n.port); string(o, "osd_name", n.osd_name); return o; }
template<class T> void optional(cJSON* o, const char* k, const std::optional<T>& v) { if (v) id(o, k, *v); else cJSON_AddNullToObject(o, k); }
template<class T, class F> void array(cJSON* o, const char* k, const std::vector<T>& values, F encoder) {
    auto* a = cJSON_AddArrayToObject(o, k); for (const auto& value : values) cJSON_AddItemToArray(a, encoder(value));
}
template<class T> void ids(cJSON* o, const char* k, const std::vector<T>& values) {
    array(o, k, values, [](const T& v) { return cJSON_CreateString(v.str().c_str()); });
}
template<class T> bool id_list(cJSON* o, const char* k, std::vector<T>& values, size_t maximum) {
    return list(o, k, values, maximum, [](cJSON* v, T& out) { if (!cJSON_IsString(v)) return false; auto p = T::parse(v->valuestring); if (!p) return false; out = *p; return true; });
}
bool signal_port(cJSON* o, model::SignalPort& p) {
    uint64_t index;
    if (!keys(o, {"component_id", "port_index", "direction"}) || !identifier(o, "component_id", p.component_id) || !number(o, "port_index", index, 31) ||
        !enumeration(o, "direction", p.direction, {{"input", model::SignalDirection::input}, {"output", model::SignalDirection::output}})) return false;
    p.port_index = static_cast<uint8_t>(index); return true;
}
cJSON* encode_signal_port(const model::SignalPort& p) {
    auto* o = cJSON_CreateObject(); id(o, "component_id", p.component_id); cJSON_AddNumberToObject(o, "port_index", p.port_index);
    cJSON_AddStringToObject(o, "direction", p.direction == model::SignalDirection::input ? "input" : "output"); return o;
}
bool demand(cJSON* o, model::ResourceDemand& d) {
    std::optional<model::OctopusId> owner;
    if (!keys(o, {"resource_id", "mode", "control_owner"}) || !identifier(o, "resource_id", d.resource_id) || !text(o, "mode", d.mode, 64) || !optional_id(o, "control_owner", owner)) return false;
    d.control_owner = owner.value_or(model::OctopusId{}); return true;
}
cJSON* encode_demand(const model::ResourceDemand& d) {
    auto* o = cJSON_CreateObject(); id(o, "resource_id", d.resource_id); string(o, "mode", d.mode);
    if (d.control_owner) id(o, "control_owner", d.control_owner); else cJSON_AddNullToObject(o, "control_owner"); return o;
}
bool bytes(cJSON* o, const char* key, std::vector<uint8_t>& out) {
    return list(o, key, out, 15, [](cJSON* v, uint8_t& byte) {
        if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble != v->valueint || v->valueint < 0 || v->valueint > 255) return false;
        byte = static_cast<uint8_t>(v->valueint); return true;
    });
}
bool step(cJSON* o, model::PlanStep& s) {
    if (!keys(o, {"binding_id", "operation", "arguments", "dependencies", "require_observation"}) || !identifier(o, "binding_id", s.binding_id) ||
        !enumeration(o, "operation", s.operation, {{"ensure_power", model::OperationKind::ensure_power}, {"select_path", model::OperationKind::select_path},
            {"announce_active_source", model::OperationKind::announce_active_source}, {"press_key", model::OperationKind::press_key},
            {"release_key", model::OperationKind::release_key}, {"query_state", model::OperationKind::query_state}}) ||
        !boolean(o, "require_observation", s.require_observation)) return false;
    auto* a = field(o, "arguments"); uint64_t key, path, query;
    if (!keys(a, {"on", "key", "physical_path", "query_opcode"}) || !boolean(a, "on", s.arguments.on) || !number(a, "key", key, 0x76) ||
        !number(a, "physical_path", path, 65535) || !number(a, "query_opcode", query, 255)) return false;
    s.arguments.key = static_cast<uint8_t>(key); s.arguments.physical_path = static_cast<uint16_t>(path); s.arguments.query_opcode = static_cast<uint8_t>(query);
    return list(o, "dependencies", s.dependencies, 32, [](cJSON* v, size_t& index) {
        if (!cJSON_IsNumber(v) || v->valuedouble != v->valueint || v->valueint < 0 || v->valueint >= 64) return false;
        index = static_cast<size_t>(v->valueint); return true;
    });
}
cJSON* encode_step(const model::PlanStep& s) {
    auto* o = cJSON_CreateObject(); id(o, "binding_id", s.binding_id); cJSON_AddStringToObject(o, "operation", operation_name(s.operation));
    auto* a = cJSON_AddObjectToObject(o, "arguments"); cJSON_AddBoolToObject(a, "on", s.arguments.on); cJSON_AddNumberToObject(a, "key", s.arguments.key);
    cJSON_AddNumberToObject(a, "physical_path", s.arguments.physical_path); cJSON_AddNumberToObject(a, "query_opcode", s.arguments.query_opcode);
    array(o, "dependencies", s.dependencies, [](size_t index) { return cJSON_CreateNumber(static_cast<double>(index)); });
    cJSON_AddBoolToObject(o, "require_observation", s.require_observation); return o;
}
std::string printed(cJSON* root) { char* s = cJSON_PrintUnformatted(root); if (!s) return {}; std::string out(s); cJSON_free(s); return out; }
bool label(cJSON* o, std::string& value, std::string& generated, bool& override) {
    return text(o, "label", value) && text(o, "generated_label", generated, 128, true) && boolean(o, "label_override", override, true);
}
template<class T> const T* by_id(const std::vector<T>& values, const std::function<bool(const T&)>& match) {
    auto it = std::find_if(values.begin(), values.end(), match); return it == values.end() ? nullptr : &*it;
}
struct LegacyRemoteTag {};
using LegacyRemoteId=model::Id<LegacyRemoteTag>;
struct LegacyRemote {
    LegacyRemoteId id; model::OctopusId owner; model::ZoneId zone;
    uint64_t revision=0;std::string label,generated;bool overridden=false;
    std::vector<std::optional<model::ActivityId>> buttons;
};
bool legacy_remote_data(cJSON* root,const ConfigurationV2& c,std::string& encoded) {
    std::vector<LegacyRemote> records;
    if(field(root,"remotes")&&!list(root,"remotes",records,64,[](cJSON* o,LegacyRemote& r){
        if(!keys(o,{"remote_id","owner_octopus_id","zone_id","binding_revision","label","generated_label","label_override","activity_buttons"})||!identifier(o,"remote_id",r.id)||!identifier(o,"owner_octopus_id",r.owner)||!identifier(o,"zone_id",r.zone)||!revision(o,"binding_revision",r.revision)||!label(o,r.label,r.generated,r.overridden))return false;
        return list(o,"activity_buttons",r.buttons,16,[](cJSON* value,std::optional<model::ActivityId>& id){if(cJSON_IsNull(value)){id.reset();return true;}if(!cJSON_IsString(value)||!value->valuestring)return false;id=model::ActivityId::parse(value->valuestring);return bool(id);});
    }))return false;
    std::set<LegacyRemoteId> ids;std::set<model::OctopusId> owners;
    if(c.controller_octopus_id&&records.size()>1)return false;
    for(const auto& r:records){
        if(!ids.insert(r.id).second||!owners.insert(r.owner).second||(c.controller_octopus_id&&r.owner!=*c.controller_octopus_id)||std::none_of(c.octopuses.begin(),c.octopuses.end(),[&](const auto& b){return b.octopus_id==r.owner;})||std::none_of(c.zones.begin(),c.zones.end(),[&](const auto& z){return z.zone_id==r.zone;}))return false;
        for(const auto& button:r.buttons)if(button&&std::none_of(c.activities.begin(),c.activities.end(),[&](const auto& a){return a.activity_id==*button&&a.zone_id==r.zone;}))return false;
    }
    std::optional<LegacyRemoteId> primary;
    if(field(root,"primary_remote_id")){LegacyRemoteId id;if(!identifier(root,"primary_remote_id",id)||std::none_of(records.begin(),records.end(),[&](const auto& r){return r.id==id&&r.zone==c.primary_zone_id;}))return false;primary=id;}
    if(records.empty()&&!primary){encoded.clear();return true;}
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.id<b.id;});auto* legacy=cJSON_CreateObject();
    if(primary)id(legacy,"primary_remote_id",*primary);
    if(!records.empty())array(legacy,"remotes",records,[](const LegacyRemote& r){auto* o=cJSON_CreateObject();id(o,"remote_id",r.id);id(o,"owner_octopus_id",r.owner);id(o,"zone_id",r.zone);cJSON_AddStringToObject(o,"binding_revision",model::format_uint64(r.revision).c_str());cJSON_AddStringToObject(o,"label",r.label.c_str());cJSON_AddStringToObject(o,"generated_label",r.generated.c_str());cJSON_AddBoolToObject(o,"label_override",r.overridden);array(o,"activity_buttons",r.buttons,[](const auto& value){return value?cJSON_CreateString(value->str().c_str()):cJSON_CreateNull();});return o;});
    encoded=printed(legacy);cJSON_Delete(legacy);return !encoded.empty();
}

}

ConfigurationV2 configuration_owner_slice(const ConfigurationV2& source, model::OctopusId owner) {
    auto out = source;
    std::set<model::BindingId> bindings;
    std::set<model::ComponentId> components;
    std::set<model::ResourceId> resources;
    std::set<model::RouteId> routes;
    // Actuators receive only their native CEC execution dependencies. Home
    // integration and user-input projection belong to the original main.
    for (const auto& b : source.binding_policies) if (b.port.octopus_id == owner) bindings.insert(b.binding_id);
    out.legacy_remote_configuration.clear();
    for (const auto& r : source.routes) {
        bool relevant = false;
        for (auto b : r.required_bindings) if (has_config(bindings, b)) relevant = true;
        for (const auto& d : r.demands) for (const auto& resource : source.resources) if (resource.resource_id == d.resource_id && resource.owner_octopus_id == owner) relevant = true;
        if (relevant) routes.insert(r.route_id);
    }
    retain_config(out.routes, [&](const auto& r) { return has_config(routes, r.route_id); });
    for (const auto& r : out.routes) {
        components.insert(r.source); components.insert(r.display);
        for (auto b : r.required_bindings) bindings.insert(b);
        for (const auto& p : r.physical_path) components.insert(p.component_id);
        for (const auto& d : r.demands) resources.insert(d.resource_id);
    }
    retain_config(out.activities, [&](const auto& a) { return has_config(routes, a.route_id); });
    std::set<model::ActivityId> activities;
    for (const auto& a : out.activities) { activities.insert(a.activity_id); for (auto ref : {a.source,a.display,a.audio,a.switch_device}) if (ref) components.insert(*ref); }
    retain_config(out.activity_order,[&](const auto& id){return has_config(activities,id);});
    retain_config(out.binding_policies, [&](const auto& b) { return has_config(bindings, b.binding_id); });
    std::set<model::DomainId> domains;
    for (const auto& b : out.binding_policies) { components.insert(b.component_id); domains.insert(b.domain_id); if (b.proxy_identity_resource_id) resources.insert(*b.proxy_identity_resource_id); }
    retain_config(out.domains, [&](const auto& d) { return has_config(domains,d.domain_id) || std::any_of(d.ports.begin(),d.ports.end(),[&](const auto& p){return p.port.octopus_id==owner;}); });
    retain_config(out.components, [&](const auto& item) { return has_config(components,item.record.component_id); });
    retain_config(out.resources, [&](const auto& r) { return has_config(resources,r.resource_id); });
    retain_config(out.signal_edges,[&](const auto& edge){for(const auto& r:out.routes)for(size_t n=1;n<r.physical_path.size();++n)if(r.physical_path[n-1]==edge.from&&r.physical_path[n]==edge.to)return true;return false;});
    for(auto& z:out.zones)retain_config(z.activity_order,[&](const auto& id){return has_config(activities,id);});
    retain_config(out.zones,[&](const auto& z){return z.zone_id==out.primary_zone_id||!z.activity_order.empty();});
    retain_config(out.external_slots,[&](const auto& slot){return slot.tombstone||has_config(activities,slot.activity_id);});
    retain_config(out.quirks,[&](const auto& q){return has_config(components,q.component_id);});
    std::set<model::OctopusId> boards{owner};
    if(out.controller_octopus_id)boards.insert(*out.controller_octopus_id);
    for(const auto& d:out.domains){boards.insert(d.owner_octopus_id);for(const auto& p:d.ports)boards.insert(p.port.octopus_id);}
    for(const auto& item:out.components)for(const auto& n:item.record.qualified_names)boards.insert(n.port.octopus_id);
    for(const auto& r:out.resources)boards.insert(r.owner_octopus_id);
    retain_config(out.octopuses,[&](const auto& b){return has_config(boards,b.octopus_id);});
    // A newly enrolled owner with no assigned equipment receives only passive
    // monitor ports; this metadata never declares an electrical attachment.
    if(std::none_of(out.octopuses.begin(),out.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;}))out.octopuses.push_back({owner,"Amity","Amity",false,1});
    return out;
}

bool parse_configuration_v2(const std::string& input, ConfigurationV2& out, std::string& error) {
    auto root = parse(input); ConfigurationV2 c; uint64_t version;
    auto fail = [&] { error = "Invalid schema 2 field, identifier, bounded list, or canonical revision"; return false; };
    if (!root || !keys(root.get(), {"version", "consortium_id", "config_revision", "presentation_revision", "primary_zone_id", "octopuses", "components", "domains", "binding_policies", "resources", "signal_edges", "routes", "activities", "zones", "external_slots", "quirks", "remotes", "primary_remote_id", "controller_octopus_id", "activity_order"}) ||
        !number(root.get(), "version", version, 2) || version != 2 || !identifier(root.get(), "consortium_id", c.consortium_id) ||
        !revision(root.get(), "config_revision", c.config_revision) || !revision(root.get(), "presentation_revision", c.presentation_revision) || !identifier(root.get(), "primary_zone_id", c.primary_zone_id)) return fail();
    if (!list(root.get(), "octopuses", c.octopuses, 64, [](cJSON* o, ConfiguredOctopus& b) {
        uint64_t cap;
        if (!keys(o, {"octopus_id", "label", "generated_label", "label_override", "supported_tentacles"}) || !identifier(o, "octopus_id", b.octopus_id) || !label(o, b.label, b.generated_label, b.label_override) || !number(o, "supported_tentacles", cap, amity::max_tentacles) || !cap) return false;
        b.supported_tentacles = static_cast<uint8_t>(cap); return true;
    }) || !list(root.get(), "components", c.components, 64, [](cJSON* o, ConfiguredComponent& v) {
        if (!keys(o, {"component_id", "label", "generated_label", "label_override", "kind", "qualified_names", "capabilities"}) || !identifier(o, "component_id", v.record.component_id) ||
            !label(o, v.record.label, v.generated_label, v.label_override) || !text(o, "kind", v.kind, 16) || !list(o, "qualified_names", v.record.qualified_names, 16, name)) return false;
        return list(o, "capabilities", v.capabilities, 16, [](cJSON* a, ConfiguredCapability& cap) {
            return keys(a, {"capability", "support"}) && capability(a, cap.capability) && enumeration(a, "support", cap.support,
                {{"supported", model::Support::supported}, {"unsupported", model::Support::unsupported}, {"unknown", model::Support::unknown}});
        });
    }) || !list(root.get(), "domains", c.domains, 64, [](cJSON* o, ConfiguredDomain& d) {
        if (!keys(o, {"domain_id", "domain_revision", "owner_octopus_id", "confirmed", "ports"}) || !identifier(o, "domain_id", d.domain_id) ||
            !revision(o, "domain_revision", d.domain_revision) || !identifier(o, "owner_octopus_id", d.owner_octopus_id) || !boolean(o, "confirmed", d.confirmed)) return false;
        return list(o, "ports", d.ports, 80, [&](cJSON* a, ConfiguredPort& p) {
            if (!keys(a, {"port", "role", "physical_address"}) || !port(field(a, "port"), p.port) || !role(a, p.role)) return false;
            auto* pa = field(a, "physical_address"); if (!pa || cJSON_IsNull(pa)) return true;
            model::PhysicalAddressEvidence evidence; uint64_t value;
            if (!keys(pa, {"value", "provenance", "confirmed"}) || !number(pa, "value", value, 65535) || !boolean(pa, "confirmed", evidence.confirmed) ||
                !enumeration(pa, "provenance", evidence.provenance, {{"unknown", model::PaProvenance::unknown}, {"legacy_minimum_hint", model::PaProvenance::legacy_minimum_hint}, {"user_confirmed_fallback", model::PaProvenance::user_confirmed_fallback}})) return false;
            evidence.attachment = p.port; evidence.domain_id = d.domain_id; evidence.domain_revision = d.domain_revision; evidence.physical_address = static_cast<uint16_t>(value); p.physical_address = evidence; return true;
        });
    }) || !list(root.get(), "binding_policies", c.binding_policies, 128, [](cJSON* o, BindingPolicy& b) {
        if (!keys(o, {"binding_id", "component_id", "capability", "kind", "domain_id", "port", "device_name", "proxy_identity_resource_id", "discrete_power_on", "discrete_power_off", "image_view_on", "exact_path_selection"}) ||
            !identifier(o, "binding_id", b.binding_id) || !identifier(o, "component_id", b.component_id) || !capability(o, b.capability) || !identifier(o, "domain_id", b.domain_id) || !port(field(o, "port"), b.port) ||
            !enumeration(o, "kind", b.kind, {{"native_appliance", model::BindingKind::native_appliance}, {"proxy_representation", model::BindingKind::proxy_representation}}) ||
            !optional_id(o, "proxy_identity_resource_id", b.proxy_identity_resource_id) || !boolean(o, "discrete_power_on", b.discrete_power_on) ||
            !boolean(o, "discrete_power_off", b.discrete_power_off) || !boolean(o, "image_view_on", b.image_view_on) || !boolean(o, "exact_path_selection", b.exact_path_selection)) return false;
        auto* n = field(o, "device_name"); if (n && !cJSON_IsNull(n)) { model::QualifiedDeviceName value; if (!name(n, value)) return false; b.device_name = value; } return true;
    }) || !list(root.get(), "resources", c.resources, 128, [](cJSON* o, model::Resource& r) {
        return keys(o, {"resource_id", "owner_octopus_id", "compatibility", "supported_modes"}) && identifier(o, "resource_id", r.resource_id) && identifier(o, "owner_octopus_id", r.owner_octopus_id) &&
            enumeration(o, "compatibility", r.compatibility, {{"same_mode", model::ResourceCompatibility::same_mode}, {"exclusive", model::ResourceCompatibility::exclusive}, {"control_owner", model::ResourceCompatibility::control_owner}}) &&
            list(o, "supported_modes", r.supported_modes, 32, [](cJSON* v, std::string& s) { if (!cJSON_IsString(v) || !v->valuestring || !*v->valuestring || std::strlen(v->valuestring) > 64) return false; s = v->valuestring; return true; });
    }) || !list(root.get(), "signal_edges", c.signal_edges, 256, [](cJSON* o, model::SignalEdge& e) {
        return keys(o, {"from", "to", "kind", "support"}) && signal_port(field(o, "from"), e.from) && signal_port(field(o, "to"), e.to) &&
            enumeration(o, "kind", e.kind, {{"hdmi_cable", model::SignalEdgeKind::hdmi_cable}, {"internal_equipment_path", model::SignalEdgeKind::internal_equipment_path}}) &&
            enumeration(o, "support", e.support, {{"supported", model::Support::supported}, {"unsupported", model::Support::unsupported}, {"unknown", model::Support::unknown}});
    }) || !list(root.get(), "routes", c.routes, 64, [](cJSON* o, model::RouteDefinition& r) {
        return keys(o, {"route_id", "source", "display", "physical_path", "demands", "required_bindings", "steps", "requires_return_audio", "return_audio_support", "changes_physical_addresses", "qualified_physical_address_stability"}) &&
            identifier(o, "route_id", r.route_id) && identifier(o, "source", r.source) && identifier(o, "display", r.display) && list(o, "physical_path", r.physical_path, 32, signal_port) &&
            list(o, "demands", r.demands, 32, demand) && id_list(o, "required_bindings", r.required_bindings, 32) && list(o, "steps", r.steps, 64, step) &&
            boolean(o, "requires_return_audio", r.requires_return_audio) && enumeration(o, "return_audio_support", r.return_audio_support, {{"supported", model::Support::supported}, {"unsupported", model::Support::unsupported}, {"unknown", model::Support::unknown}}) &&
            boolean(o, "changes_physical_addresses", r.changes_physical_addresses) && boolean(o, "qualified_physical_address_stability", r.qualified_physical_address_stability);
    }) || !list(root.get(), "activities", c.activities, 64, [](cJSON* o, ConfiguredActivity& a) {
        uint64_t input;
        if (!keys(o, {"activity_id", "zone_id", "route_id", "label", "generated_label", "label_override", "enabled", "source", "display", "audio", "switch_device", "switch_input"}) ||
            !identifier(o, "activity_id", a.activity_id) || !identifier(o, "zone_id", a.zone_id) || !identifier(o, "route_id", a.route_id) || !label(o, a.label, a.generated_label, a.label_override) ||
            !boolean(o, "enabled", a.enabled) || !optional_id(o, "source", a.source) || !optional_id(o, "display", a.display) || !optional_id(o, "audio", a.audio) || !optional_id(o, "switch_device", a.switch_device) || !number(o, "switch_input", input, 255)) return false;
        a.switch_input = static_cast<uint8_t>(input); return true;
    }) || !list(root.get(), "zones", c.zones, 16, [](cJSON* o, ConfiguredZone& z) {
        return keys(o, {"zone_id", "label", "generated_label", "label_override", "activity_order"}) && identifier(o, "zone_id", z.zone_id) && label(o, z.label, z.generated_label, z.label_override) && id_list(o, "activity_order", z.activity_order, 64);
    }) || !list(root.get(), "external_slots", c.external_slots, 256, [](cJSON* o, ExternalSlot& s) {
        uint64_t index;
        if (!keys(o, {"slot", "activity_id", "tombstone", "hidden", "alias"}) || !number(o, "slot", index, 65535) || !identifier(o, "activity_id", s.activity_id) ||
            !boolean(o, "tombstone", s.tombstone) || !boolean(o, "hidden", s.hidden) || !text(o, "alias", s.alias, 128)) return false;
        s.slot = static_cast<uint16_t>(index); return true;
    }) || !list(root.get(), "quirks", c.quirks, 32, [](cJSON* o, ScopedQuirk& q) {
        uint64_t vendor;
        if (!keys(o, {"component_id", "vendor_id", "power_on", "power_off"}) || !identifier(o, "component_id", q.component_id) || !number(o, "vendor_id", vendor, 0xffffff) || !bytes(o, "power_on", q.power_on) || !bytes(o, "power_off", q.power_off)) return false;
        q.vendor_id = static_cast<uint32_t>(vendor); return true;
    })) return fail();
    if(field(root.get(),"controller_octopus_id")){model::OctopusId controller;if(!identifier(root.get(),"controller_octopus_id",controller))return fail();c.controller_octopus_id=controller;}
    if(field(root.get(),"activity_order")&&!id_list(root.get(),"activity_order",c.activity_order,64))return fail();
    if(!legacy_remote_data(root.get(),c,c.legacy_remote_configuration))return fail();
    if (!validate_configuration_v2(c, error)) return false;
    out = std::move(c); error.clear(); return true;
}

std::string serialize_configuration_v2(const ConfigurationV2& input, bool executable) {
    auto c = input;
    const auto sort_by = [](auto& values, auto key) { std::sort(values.begin(), values.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); }); };
    sort_by(c.components, [](const auto& v) { return v.record.component_id; }); sort_by(c.domains, [](const auto& v) { return v.domain_id; });
    sort_by(c.binding_policies, [](const auto& v) { return v.binding_id; }); sort_by(c.resources, [](const auto& v) { return v.resource_id; });
    sort_by(c.routes, [](const auto& v) { return v.route_id; }); sort_by(c.activities, [](const auto& v) { return v.activity_id; });
    sort_by(c.zones, [](const auto& v) { return v.zone_id; }); sort_by(c.external_slots, [](const auto& v) { return v.slot; });
    sort_by(c.octopuses, [](const auto& v) { return v.octopus_id; });
    Json root(cJSON_CreateObject(), cJSON_Delete); cJSON_AddNumberToObject(root.get(), "version", 2); id(root.get(), "consortium_id", c.consortium_id);
    if (!executable) { rev(root.get(), "config_revision", c.config_revision); rev(root.get(), "presentation_revision", c.presentation_revision); }
    id(root.get(), "primary_zone_id", c.primary_zone_id);
    if(c.controller_octopus_id)id(root.get(),"controller_octopus_id",*c.controller_octopus_id);
    if(!executable&&!c.activity_order.empty())ids(root.get(),"activity_order",c.activity_order);
    Json legacy(c.legacy_remote_configuration.empty()?nullptr:cJSON_Parse(c.legacy_remote_configuration.c_str()),cJSON_Delete);
    if(legacy&&field(legacy.get(),"primary_remote_id"))cJSON_AddItemToObject(root.get(),"primary_remote_id",cJSON_Duplicate(field(legacy.get(),"primary_remote_id"),true));
    const auto labels = [&](cJSON* o, const std::string& value, const std::string& generated, bool overridden) {
        if (!executable) { string(o, "label", value); string(o, "generated_label", generated); cJSON_AddBoolToObject(o, "label_override", overridden); }
    };
    array(root.get(), "octopuses", c.octopuses, [&](const ConfiguredOctopus& b) {
        auto* o = cJSON_CreateObject(); id(o, "octopus_id", b.octopus_id); labels(o, b.label, b.generated_label, b.label_override); cJSON_AddNumberToObject(o, "supported_tentacles", b.supported_tentacles); return o;
    });
    array(root.get(), "components", c.components, [&](const ConfiguredComponent& v) {
        auto* o = cJSON_CreateObject(); id(o, "component_id", v.record.component_id); labels(o, v.record.label, v.generated_label, v.label_override); string(o, "kind", v.kind);
        array(o, "qualified_names", v.record.qualified_names, encode_name);
        auto capabilities = v.capabilities; sort_by(capabilities, [](const auto& a) { return a.capability; });
        array(o, "capabilities", capabilities, [](const ConfiguredCapability& cap) {
            auto* a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "capability", capability_name(cap.capability)); cJSON_AddStringToObject(a, "support", cap.support == model::Support::supported ? "supported" : cap.support == model::Support::unsupported ? "unsupported" : "unknown"); return a;
        }); return o;
    });
    array(root.get(), "domains", c.domains, [&](const ConfiguredDomain& d) {
        auto* o = cJSON_CreateObject(); id(o, "domain_id", d.domain_id); rev(o, "domain_revision", d.domain_revision); id(o, "owner_octopus_id", d.owner_octopus_id); cJSON_AddBoolToObject(o, "confirmed", d.confirmed);
        auto ports = d.ports; sort_by(ports, [](const auto& p) { return p.port.octopus_id.str() + std::to_string(p.port.tentacle_id) + model::side_name(p.port.side); });
        array(o, "ports", ports, [](const ConfiguredPort& p) {
            auto* a = cJSON_CreateObject(); cJSON_AddItemToObject(a, "port", encode_port(p.port)); cJSON_AddStringToObject(a, "role", role_name(p.role));
            if (!p.physical_address) cJSON_AddNullToObject(a, "physical_address");
            else {
                auto* pa = cJSON_AddObjectToObject(a, "physical_address"); cJSON_AddNumberToObject(pa, "value", p.physical_address->physical_address);
                cJSON_AddStringToObject(pa, "provenance", p.physical_address->provenance == model::PaProvenance::user_confirmed_fallback ? "user_confirmed_fallback" : p.physical_address->provenance == model::PaProvenance::legacy_minimum_hint ? "legacy_minimum_hint" : "unknown");
                cJSON_AddBoolToObject(pa, "confirmed", p.physical_address->confirmed);
            } return a;
        }); return o;
    });
    array(root.get(), "binding_policies", c.binding_policies, [](const BindingPolicy& b) {
        auto* o = cJSON_CreateObject(); id(o, "binding_id", b.binding_id); id(o, "component_id", b.component_id); cJSON_AddStringToObject(o, "capability", capability_name(b.capability));
        cJSON_AddStringToObject(o, "kind", b.kind == model::BindingKind::native_appliance ? "native_appliance" : "proxy_representation"); id(o, "domain_id", b.domain_id); cJSON_AddItemToObject(o, "port", encode_port(b.port));
        if (b.device_name) cJSON_AddItemToObject(o, "device_name", encode_name(*b.device_name)); else cJSON_AddNullToObject(o, "device_name"); optional(o, "proxy_identity_resource_id", b.proxy_identity_resource_id);
        cJSON_AddBoolToObject(o, "discrete_power_on", b.discrete_power_on); cJSON_AddBoolToObject(o, "discrete_power_off", b.discrete_power_off); cJSON_AddBoolToObject(o, "image_view_on", b.image_view_on); cJSON_AddBoolToObject(o, "exact_path_selection", b.exact_path_selection); return o;
    });
    array(root.get(), "resources", c.resources, [&](const model::Resource& r) {
        auto* o = cJSON_CreateObject(); id(o, "resource_id", r.resource_id); id(o, "owner_octopus_id", r.owner_octopus_id);
        cJSON_AddStringToObject(o, "compatibility", r.compatibility == model::ResourceCompatibility::same_mode ? "same_mode" : r.compatibility == model::ResourceCompatibility::exclusive ? "exclusive" : "control_owner");
        auto modes = r.supported_modes; std::sort(modes.begin(), modes.end()); array(o, "supported_modes", modes, [](const std::string& s) { return cJSON_CreateString(s.c_str()); }); return o;
    });
    array(root.get(), "signal_edges", c.signal_edges, [](const model::SignalEdge& e) {
        auto* o = cJSON_CreateObject(); cJSON_AddItemToObject(o, "from", encode_signal_port(e.from)); cJSON_AddItemToObject(o, "to", encode_signal_port(e.to));
        cJSON_AddStringToObject(o, "kind", e.kind == model::SignalEdgeKind::hdmi_cable ? "hdmi_cable" : "internal_equipment_path");
        cJSON_AddStringToObject(o, "support", e.support == model::Support::supported ? "supported" : e.support == model::Support::unsupported ? "unsupported" : "unknown"); return o;
    });
    array(root.get(), "routes", c.routes, [](const model::RouteDefinition& r) {
        auto* o = cJSON_CreateObject(); id(o, "route_id", r.route_id); id(o, "source", r.source); id(o, "display", r.display);
        array(o, "physical_path", r.physical_path, encode_signal_port); array(o, "demands", r.demands, encode_demand); ids(o, "required_bindings", r.required_bindings); array(o, "steps", r.steps, encode_step);
        cJSON_AddBoolToObject(o, "requires_return_audio", r.requires_return_audio); cJSON_AddStringToObject(o, "return_audio_support", r.return_audio_support == model::Support::supported ? "supported" : r.return_audio_support == model::Support::unsupported ? "unsupported" : "unknown");
        cJSON_AddBoolToObject(o, "changes_physical_addresses", r.changes_physical_addresses); cJSON_AddBoolToObject(o, "qualified_physical_address_stability", r.qualified_physical_address_stability); return o;
    });
    array(root.get(), "activities", c.activities, [&](const ConfiguredActivity& a) {
        auto* o = cJSON_CreateObject(); id(o, "activity_id", a.activity_id); id(o, "zone_id", a.zone_id); id(o, "route_id", a.route_id); labels(o, a.label, a.generated_label, a.label_override); cJSON_AddBoolToObject(o, "enabled", a.enabled);
        optional(o, "source", a.source); optional(o, "display", a.display); optional(o, "audio", a.audio); optional(o, "switch_device", a.switch_device); cJSON_AddNumberToObject(o, "switch_input", a.switch_input); return o;
    });
    array(root.get(), "zones", c.zones, [&](const ConfiguredZone& z) { auto* o = cJSON_CreateObject(); id(o, "zone_id", z.zone_id); labels(o, z.label, z.generated_label, z.label_override); if (!executable) ids(o, "activity_order", z.activity_order); return o; });
    array(root.get(), "external_slots", c.external_slots, [&](const ExternalSlot& s) {
        auto* o = cJSON_CreateObject(); cJSON_AddNumberToObject(o, "slot", s.slot); id(o, "activity_id", s.activity_id); cJSON_AddBoolToObject(o, "tombstone", s.tombstone);
        if (!executable) { cJSON_AddBoolToObject(o, "hidden", s.hidden); string(o, "alias", s.alias); } return o;
    });
    array(root.get(), "quirks", c.quirks, [](const ScopedQuirk& q) {
        auto* o = cJSON_CreateObject(); id(o, "component_id", q.component_id); cJSON_AddNumberToObject(o, "vendor_id", q.vendor_id);
        array(o, "power_on", q.power_on, [](uint8_t b) { return cJSON_CreateNumber(b); }); array(o, "power_off", q.power_off, [](uint8_t b) { return cJSON_CreateNumber(b); }); return o;
    });
    // Omit an empty optional extension so pre-extension committed schema2
    // records remain byte-for-byte canonical and recover without a rewrite.
    if(legacy&&field(legacy.get(),"remotes"))cJSON_AddItemToObject(root.get(),"remotes",cJSON_Duplicate(field(legacy.get(),"remotes"),true));
    return printed(root.get());
}

std::string sha256_hex(std::string_view input) {
    unsigned char digest[32];
#ifdef ESP_PLATFORM
    size_t length = 0;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_compute(PSA_ALG_SHA_256,
        reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, sizeof(digest), &length) != PSA_SUCCESS || length != sizeof(digest)) return {};
#else
    if (!SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest)) return {};
#endif
    static constexpr char hex[] = "0123456789abcdef"; std::string result(64, '0');
    for (size_t i = 0; i < 32; ++i) { result[i * 2] = hex[digest[i] >> 4]; result[i * 2 + 1] = hex[digest[i] & 15]; } return result;
}
std::string executable_policy_hash(const ConfigurationV2& c) { return sha256_hex(serialize_configuration_v2(c, true)); }
model::Topology configured_topology(const ConfigurationV2& c) {
    model::Topology t; t.config_revision = c.config_revision; for (const auto& component : c.components) t.components.push_back(component.record);
    t.resources = c.resources; t.signal_edges = c.signal_edges; t.routes = c.routes;
    for (const auto& a : c.activities) if (a.enabled) t.activities.push_back({a.activity_id, a.zone_id, a.route_id});
    return t;
}
const ConfiguredActivity* activity_for_slot(const ConfigurationV2& c, unsigned slot) {
    for (const auto& s : c.external_slots) if (s.slot == slot && !s.tombstone) for (const auto& a : c.activities) if (a.activity_id == s.activity_id) return &a;
    return nullptr;
}
std::vector<model::ActivityId> ordered_activities(const ConfigurationV2& c) {
    std::vector<model::ActivityId> result;
    const auto add=[&](model::ActivityId id){if(std::find(result.begin(),result.end(),id)==result.end()&&std::any_of(c.activities.begin(),c.activities.end(),[&](const auto& a){return a.activity_id==id;}))result.push_back(id);};
    for(const auto& id:c.activity_order)add(id);
    for(const auto& z:c.zones)if(z.zone_id==c.primary_zone_id)for(const auto& id:z.activity_order)add(id);
    for(const auto& z:c.zones)if(z.zone_id!=c.primary_zone_id)for(const auto& id:z.activity_order)add(id);
    for(const auto& a:c.activities)add(a.activity_id);
    return result;
}
bool reconcile_external_slots(const ConfigurationV2& previous, ConfigurationV2& candidate, std::string& error) {
    // A fresh installation has no external identities to protect. A deliberate
    // portable restore may initialize its exact sparse slots and tombstones.
    if(previous.activities.empty()&&previous.external_slots.empty()&&!candidate.external_slots.empty())return true;
    auto slots = previous.external_slots;
    for (auto& slot : slots) {
        bool exists = false; for (const auto& a : candidate.activities) if (a.activity_id == slot.activity_id) exists = true;
        if (!exists) slot.tombstone = true;
        for (const auto& desired : candidate.external_slots) if (desired.slot == slot.slot) {
            if (desired.activity_id != slot.activity_id) { error = "External selectors and tombstones cannot be reused"; return false; }
            if (exists) slot.tombstone = desired.tombstone;
            slot.alias = desired.alias; slot.hidden = desired.hidden;
        }
    }
    uint32_t next = 0; for (const auto& slot : slots) next = std::max(next, static_cast<uint32_t>(slot.slot) + 1);
    for (const auto& a : candidate.activities) {
        bool assigned = false; for (const auto& slot : slots) if (slot.activity_id == a.activity_id) assigned = true;
        if (!assigned && next < 16 && slots.size() < 16) slots.push_back({static_cast<uint16_t>(next++), a.activity_id, false, false, {}});
    }
    candidate.external_slots = std::move(slots); return true;
}

bool bind_original_controller(ConfigurationV2& config, model::OctopusId original, bool& changed, std::string& error) {
    changed = false;
    if (!original || !validate_configuration_v2(config, error)) {
        if (!original) error = "The original controller identity is not established";
        return false;
    }
    if (config.controller_octopus_id && *config.controller_octopus_id != original) {
        error = "This configuration belongs to a different original controller"; return false;
    }
    if (std::none_of(config.octopuses.begin(), config.octopuses.end(), [&](const auto& b) { return b.octopus_id == original; })) {
        error = "The original controller is missing from the configuration"; return false;
    }
    auto upgraded = config;
    upgraded.controller_octopus_id = original;
    upgraded.legacy_remote_configuration.clear();
    changed = serialize_configuration_v2(upgraded) != serialize_configuration_v2(config);
    config = std::move(upgraded); error.clear(); return true;
}

bool validate_configuration_v2(const ConfigurationV2& c, std::string& error) {
    const auto fail = [&](const char* reason) { error = reason; return false; };
    if (!c.consortium_id || !c.config_revision || !c.presentation_revision || !c.primary_zone_id || c.components.size() > 64 || c.domains.size() > 64 || c.binding_policies.size() > 128 ||
        c.resources.size() > 128 || c.signal_edges.size() > 256 || c.routes.size() > 64 || c.activities.size() > 64 || c.zones.empty() || c.zones.size() > 16 || c.external_slots.size() > 16 || c.quirks.size() > 32 || c.octopuses.empty() || c.octopuses.size() > 64) return fail("Configuration identifiers, revisions, or capacity invalid");
    std::set<model::ComponentId> components; std::set<model::DomainId> domains; std::set<model::BindingId> bindings; std::set<model::ResourceId> resources; std::set<model::RouteId> routes; std::set<model::ActivityId> activities; std::set<model::ZoneId> zones;
    std::set<std::string> selectors, ports;
    std::set<model::OctopusId> octopuses;
    for (const auto& b : c.octopuses) if (!b.octopus_id || !octopuses.insert(b.octopus_id).second || b.label.size() > 128 || b.generated_label.size() > 128 || !b.supported_tentacles || b.supported_tentacles > amity::max_tentacles) return fail("Invalid Amity device location or hardware profile");
    if (c.controller_octopus_id && (!*c.controller_octopus_id || !octopuses.count(*c.controller_octopus_id))) return fail("The original controller must be retained");
    const auto valid_port = [&](const model::CecPortRef& p) {
        for (const auto& b : c.octopuses) if (b.octopus_id == p.octopus_id && p.tentacle_id < b.supported_tentacles) return p.valid() && p.consortium_id == c.consortium_id;
        return false;
    };
    for (const auto& component : c.components) {
        if (!component.record.component_id || !components.insert(component.record.component_id).second || component.record.label.size() > 128 || component.generated_label.size() > 128 || component.capabilities.size() > 16 || component.record.qualified_names.size() > 16) return fail("Invalid or duplicate configured component");
        if (component.kind != "source" && component.kind != "display" && component.kind != "receiver" && component.kind != "switch" && component.kind != "audio" && component.kind != "placeholder") return fail("Unknown equipment kind");
        if (component.record.qualified_names.empty() && component.kind != "placeholder") return fail("CEC equipment must retain an exact qualified OSD selector");
        for (const auto& n : component.record.qualified_names) if (!n.valid() || !valid_port(n.port) || !selectors.insert(model::name_json(n)).second) return fail("Duplicate, foreign, or invalid native selector");
        std::set<model::Capability> caps; for (const auto& cap : component.capabilities) if (!caps.insert(cap.capability).second || std::string(capability_name(cap.capability)) == "invalid" || (cap.support != model::Support::supported && cap.support != model::Support::unsupported && cap.support != model::Support::unknown)) return fail("Duplicate or invalid capability");
    }
    for (const auto& domain : c.domains) {
        if (!domain.domain_id || !domains.insert(domain.domain_id).second || !domain.domain_revision || !octopuses.count(domain.owner_octopus_id) || domain.ports.empty() || domain.ports.size() > 80) return fail("Invalid configured electrical domain");
        unsigned tv = 0;
        for (const auto& p : domain.ports) {
            const auto key = p.port.octopus_id.str() + ":" + std::to_string(p.port.tentacle_id) + ":" + model::side_name(p.port.side);
            if (!valid_port(p.port) || !ports.insert(key).second || std::string(role_name(p.role)) == "invalid") return fail("Port belongs to several domains or has invalid scope/role");
            if (!domain.confirmed && p.role != model::PortRole::disabled && p.role != model::PortRole::monitor) return fail("Unconfirmed electrical domain cannot grant active roles");
            if (p.role == model::PortRole::tv_controller && ++tv > 1) return fail("Electrical domain may have only one virtual TV");
            if (p.physical_address && (p.physical_address->attachment != p.port || p.physical_address->domain_id != domain.domain_id || p.physical_address->domain_revision != domain.domain_revision ||
                p.physical_address->reporter || !model::valid_physical_address(p.physical_address->physical_address, false) ||
                (p.physical_address->confirmed && p.physical_address->provenance != model::PaProvenance::user_confirmed_fallback))) return fail("Persist only an attachment-specific fallback/hint, never runtime PA authority");
        }
    }
    for (const auto& resource : c.resources) {
        if (!resource.resource_id || !resources.insert(resource.resource_id).second || !resource.owner_octopus_id || resource.supported_modes.empty() || resource.supported_modes.size() > 32 ||
            (resource.compatibility != model::ResourceCompatibility::same_mode && resource.compatibility != model::ResourceCompatibility::exclusive && resource.compatibility != model::ResourceCompatibility::control_owner)) return fail("Invalid resource declaration");
        std::set<std::string> modes; for (const auto& mode : resource.supported_modes) if (mode.empty() || mode.size() > 64 || !modes.insert(mode).second) return fail("Invalid resource mode");
    }
    for (const auto& b : c.binding_policies) {
        if (!b.binding_id || !bindings.insert(b.binding_id).second || !components.count(b.component_id) || !domains.count(b.domain_id) || !b.port.valid() || b.port.consortium_id != c.consortium_id) return fail("Invalid binding policy references");
        const ConfiguredComponent* component = nullptr; const ConfiguredPort* local = nullptr;
        for (const auto& item : c.components) if (item.record.component_id == b.component_id) component = &item;
        for (const auto& domain : c.domains) if (domain.domain_id == b.domain_id) for (const auto& p : domain.ports) if (p.port == b.port) local = &p;
        if (!local || (local->role != model::PortRole::tv_controller && local->role != model::PortRole::playback_proxy)) return fail("Binding requires an explicitly controlled port");
        if (b.kind == model::BindingKind::native_appliance) {
            if (!b.device_name || b.device_name->port != b.port || b.proxy_identity_resource_id || std::find(component->record.qualified_names.begin(), component->record.qualified_names.end(), *b.device_name) == component->record.qualified_names.end()) return fail("Native binding must use its configured exact qualified name");
        } else if (b.kind == model::BindingKind::proxy_representation) {
            if (b.device_name || !b.proxy_identity_resource_id || !resources.count(*b.proxy_identity_resource_id) || local->role != model::PortRole::playback_proxy || b.capability != model::Capability::proxy_activation) return fail("Proxy representation requires a separate allocated identity resource");
        } else return fail("Unknown binding policy kind");
        bool supported = b.kind == model::BindingKind::proxy_representation;
        for (const auto& cap : component->capabilities) if (cap.capability == b.capability && cap.support == model::Support::supported) supported = true;
        if (!supported) return fail("Required binding capability is unknown or unsupported");
    }
    for (const auto& edge : c.signal_edges) {
        if (!components.count(edge.from.component_id) || !components.count(edge.to.component_id) || edge.from.port_index > 31 || edge.to.port_index > 31) return fail("Signal graph references unknown equipment");
        bool valid = edge.kind == model::SignalEdgeKind::hdmi_cable ? edge.from.direction == model::SignalDirection::output && edge.to.direction == model::SignalDirection::input :
            edge.kind == model::SignalEdgeKind::internal_equipment_path && edge.from.component_id == edge.to.component_id && edge.from.direction == model::SignalDirection::input && edge.to.direction == model::SignalDirection::output;
        if (!valid) return fail("Invalid physical/internal signal graph edge");
    }
    for (const auto& r : c.routes) {
        if (!r.route_id || !routes.insert(r.route_id).second || !components.count(r.source) || !components.count(r.display) || r.physical_path.size() < 2 || r.physical_path.size() > 32 ||
            r.physical_path.front().component_id != r.source || r.physical_path.back().component_id != r.display || r.required_bindings.size() > 32 || r.steps.size() > 64 || r.demands.size() > 32) return fail("Route has no complete bounded physical path");
        for (size_t i = 1; i < r.physical_path.size(); ++i) {
            bool exists = false; for (const auto& edge : c.signal_edges) if (edge.from == r.physical_path[i - 1] && edge.to == r.physical_path[i] && edge.support == model::Support::supported) exists = true;
            if (!exists) return fail("Route cannot invent an unverified HDMI path");
        }
        if (r.requires_return_audio && r.return_audio_support != model::Support::supported) return fail("Required return audio is unqualified");
        std::set<model::ResourceId> demands;
        for (const auto& d : r.demands) {
            if (!resources.count(d.resource_id) || !demands.insert(d.resource_id).second) return fail("Invalid or duplicate route resource demand");
            bool supported = false; for (const auto& resource : c.resources) if (resource.resource_id == d.resource_id && std::find(resource.supported_modes.begin(), resource.supported_modes.end(), d.mode) != resource.supported_modes.end()) supported = true;
            if (!supported) return fail("Route resource mode unsupported");
        }
        for (const auto& b : r.required_bindings) if (!bindings.count(b)) return fail("Route requires an unknown binding");
        for (size_t i = 0; i < r.steps.size(); ++i) {
            const auto& s = r.steps[i];
            if (std::find(r.required_bindings.begin(), r.required_bindings.end(), s.binding_id) == r.required_bindings.end() || s.dependencies.size() > 32 || s.arguments.hold_token || std::string(operation_name(s.operation)) == "invalid") return fail("Plan step lacks a configured binding or contains runtime authority");
            for (auto dependency : s.dependencies) if (dependency >= i) return fail("Plan dependency graph must be acyclic");
        }
    }
    for (const auto& z : c.zones) if (!z.zone_id || !zones.insert(z.zone_id).second || z.label.size() > 128 || z.generated_label.size() > 128 || z.activity_order.size() > 64) return fail("Invalid zone");
    if (!zones.count(c.primary_zone_id)) return fail("Primary zone is missing");
    for (const auto& a : c.activities) {
        if (!a.activity_id || !activities.insert(a.activity_id).second || !zones.count(a.zone_id) || !a.route_id || a.label.size() > 128 || a.generated_label.size() > 128) return fail("Invalid activity");
        for (const auto& ref : {a.source, a.display, a.audio, a.switch_device}) if (ref && !components.count(*ref)) return fail("Activity references missing equipment");
        if (a.enabled && (!routes.count(a.route_id) || !a.source || !a.display)) return fail("Enabled activity requires a validated source/display route");
    }
    std::set<model::ActivityId> global_order;
    if(c.activity_order.size()>64)return fail("Activity presentation order exceeds capacity");
    for(const auto& id:c.activity_order)if(!activities.count(id)||!global_order.insert(id).second)return fail("Activity presentation order contains a missing or repeated activity");
    for (const auto& z : c.zones) {
        std::set<model::ActivityId> order;
        for (const auto& id : z.activity_order) {
            bool exists = false; for (const auto& a : c.activities) if (a.activity_id == id && a.zone_id == z.zone_id) exists = true;
            if (!exists || !order.insert(id).second) return fail("Zone order contains foreign, duplicate or missing activities");
        }
        for (const auto& a : c.activities) if (a.zone_id == z.zone_id && !order.count(a.activity_id)) return fail("Every zone activity must occur in its presentation order");
    }
    std::set<uint16_t> slots; std::set<model::ActivityId> assigned;
    for (const auto& s : c.external_slots) if (s.slot >= 16 || !s.activity_id || !slots.insert(s.slot).second || !assigned.insert(s.activity_id).second || (!s.tombstone && !activities.count(s.activity_id)) || s.alias.size() > 128) return fail("Invalid external selector registry");
    for (const auto& q : c.quirks) if (!components.count(q.component_id) || q.vendor_id > 0xffffff || q.power_on.size() > 15 || q.power_off.size() > 15) return fail("Invalid scoped quirk");
    if(!c.legacy_remote_configuration.empty()){
        Json legacy(cJSON_Parse(c.legacy_remote_configuration.c_str()),cJSON_Delete);std::string canonical;
        if(!legacy||!keys(legacy.get(),{"remotes","primary_remote_id"})||!legacy_remote_data(legacy.get(),c,canonical)||canonical!=c.legacy_remote_configuration)return fail("Invalid read-only legacy remote metadata");
    }
    if (serialize_configuration_v2(c).size() > configuration_compatibility_limit) return fail("Configuration exceeds 32768-byte payload capacity");
    error.clear(); return true;
}

bool propose_adapter_connection(const ConfigurationV2& previous, model::OctopusId owner, uint8_t connector,
    std::optional<uint16_t> fallback, const std::function<bool(uint8_t*, size_t)>& random,
    ConfigurationV2& out, std::string& error) {
    if(!validate_configuration_v2(previous,error))return false;
    const auto board=std::find_if(previous.octopuses.begin(),previous.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});
    if(board==previous.octopuses.end()||connector>=board->supported_tentacles||(fallback&&!model::valid_physical_address(*fallback,false))){error="Choose an available HDMI adapter and a valid screen input";return false;}
    auto candidate=previous;
    for(auto side:{model::Side::front,model::Side::back}){
        const model::CecPortRef attachment{candidate.consortium_id,owner,connector,side};
        const auto desired=side==model::Side::front?model::PortRole::playback_proxy:model::PortRole::tv_controller;
        if(std::any_of(candidate.binding_policies.begin(),candidate.binding_policies.end(),[&](const auto& b){return b.port==attachment;})){error="This HDMI adapter already has assigned equipment. Review that equipment before changing its connection.";return false;}
        ConfiguredDomain* domain=nullptr;
        for(auto& d:candidate.domains)for(const auto& p:d.ports)if(p.port==attachment)domain=&d;
        if(domain){
            if(domain->ports.size()!=1||(domain->confirmed?domain->ports[0].role!=desired:(domain->ports[0].role!=model::PortRole::monitor&&domain->ports[0].role!=model::PortRole::disabled))){error="This HDMI adapter already has a different confirmed connection. Review its existing setup.";return false;}
            const bool changed=!domain->confirmed||domain->ports[0].role!=desired||(side==model::Side::front&&fallback&&(!domain->ports[0].physical_address||domain->ports[0].physical_address->physical_address!=*fallback));
            if(changed){if(domain->domain_revision==UINT64_MAX){error="Connection revision exhausted";return false;}++domain->domain_revision;}
        }else{
            const auto fresh=model::DomainId::generate(random);if(!fresh||std::any_of(candidate.domains.begin(),candidate.domains.end(),[&](const auto& d){return d.domain_id==*fresh;})){error="Could not allocate a fresh connection identity";return false;}
            ConfiguredDomain d;d.domain_id=*fresh;d.owner_octopus_id=owner;d.ports.push_back({attachment,model::PortRole::monitor,{}});candidate.domains.push_back(std::move(d));domain=&candidate.domains.back();
        }
        domain->confirmed=true;auto& port=domain->ports.front();port.role=desired;
        if(side==model::Side::back)port.physical_address.reset();
        if(side==model::Side::front&&fallback)port.physical_address=model::PhysicalAddressEvidence{attachment,domain->domain_id,domain->domain_revision,*fallback,model::PaProvenance::user_confirmed_fallback,{},true};
    }
    if(!validate_configuration_v2(candidate,error))return false;
    if(serialize_configuration_v2(candidate).size()>configuration_payload_limit){error="The saved configuration has insufficient space for another HDMI connection";return false;}
    out=std::move(candidate);error.clear();return true;
}

bool reconcile_hardware_profile(ConfigurationV2& configuration, model::OctopusId owner, uint8_t capacity,
    uint8_t configured_mask, const std::function<bool(uint8_t*, size_t)>&, bool& changed, std::string& error) {
    changed = false;
    if (!owner || !capacity || capacity > amity::max_tentacles || (configured_mask >> capacity) || !validate_configuration_v2(configuration,error)) {
        if(error.empty())error="Invalid HDMI adapter capacity or configured connector mask";
        return false;
    }
    auto candidate=configuration;
    auto board=std::find_if(candidate.octopuses.begin(),candidate.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});
    if(board==candidate.octopuses.end()){error="Hardware profile has no configured Amity device identity";return false;}
    // Unpopulated connectors are runtime monitor templates, not persisted
    // electrical claims. Creating unused domain records would exceed
    // the 16KiB mutation budget on an existing six-activity installation.
    if(board->supported_tentacles<capacity){board->supported_tentacles=capacity;changed=true;}
    if(!validate_configuration_v2(candidate,error))return false;
    configuration=std::move(candidate);error.clear();return true;
}

} // namespace config_store
