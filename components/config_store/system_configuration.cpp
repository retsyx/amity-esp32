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
std::string printed(cJSON* root) { char* s = cJSON_PrintUnformatted(root); if (!s) return {}; std::string out(s); cJSON_free(s); return out; }
bool label(cJSON* o, std::string& value, std::string& generated, bool& override) {
    return text(o, "label", value) && text(o, "generated_label", generated, 128, true) && boolean(o, "label_override", override, true);
}

void migrate_missing_selectors(ConfigurationV2& config) {
    if (!config.controller_octopus_id) return;
    for (auto& c : config.components) if (c.record.qualified_names.empty()) {
        auto side = model::Side::back;
        for (const auto& a : config.activities) if (a.display == c.record.component_id) { side = model::Side::front; break; }
        model::QualifiedDeviceName selected{{config.consortium_id, *config.controller_octopus_id, 0, side},
            c.generated_label.empty() ? c.record.label : c.generated_label};
        // A missing/non-CEC legacy source can still use the original switch
        // fallback. Keep its full label even when it is not a legal CEC OSD name.
        if (selected.valid()) c.record.qualified_names.push_back(std::move(selected));
    }
}
}

bool parse_configuration_v2(const std::string& input, ConfigurationV2& out, std::string& error) {
    auto root=parse(input);ConfigurationV2 c;uint64_t version=0;
    const auto fail=[&]{error="Invalid saved activity field, selector, identifier or revision";return false;};
    if(!root||!number(root.get(),"version",version,3)||(version!=2&&version!=3))return fail();
    const bool legacy=version==2;
    if(!(legacy?keys(root.get(),{"version","consortium_id","config_revision","presentation_revision","primary_zone_id","controller_octopus_id","activity_order","octopuses","components","domains","binding_policies","resources","signal_edges","routes","activities","zones","external_slots","remotes","primary_remote_id"}):keys(root.get(),{"version","consortium_id","config_revision","presentation_revision","primary_zone_id","controller_octopus_id","activity_order","octopuses","components","activities","zones","external_slots"}))||
        !identifier(root.get(),"consortium_id",c.consortium_id)||!revision(root.get(),"config_revision",c.config_revision)||!revision(root.get(),"presentation_revision",c.presentation_revision)||!identifier(root.get(),"primary_zone_id",c.primary_zone_id))return fail();
    if(!list(root.get(),"octopuses",c.octopuses,64,[](cJSON* o,ConfiguredOctopus& b){uint64_t capacity=1;return keys(o,{"octopus_id","label","generated_label","label_override","supported_tentacles"})&&identifier(o,"octopus_id",b.octopus_id)&&label(o,b.label,b.generated_label,b.label_override)&&(!field(o,"supported_tentacles")||number(o,"supported_tentacles",capacity,amity::max_tentacles))&&(b.supported_tentacles=static_cast<uint8_t>(capacity),true);})||
       !list(root.get(),"components",c.components,64,[&](cJSON* o,ConfiguredComponent& v){return (legacy?keys(o,{"component_id","label","generated_label","label_override","kind","qualified_names","capabilities"}):keys(o,{"component_id","label","generated_label","label_override","kind","qualified_names"}))&&identifier(o,"component_id",v.record.component_id)&&label(o,v.record.label,v.generated_label,v.label_override)&&text(o,"kind",v.kind,32)&&list(o,"qualified_names",v.record.qualified_names,16,[](cJSON* n,model::QualifiedDeviceName& selected){return name(n,selected);});})||
       !list(root.get(),"activities",c.activities,64,[&](cJSON* o,ConfiguredActivity& a){uint64_t input=0;if(!(legacy?keys(o,{"activity_id","zone_id","route_id","label","generated_label","label_override","enabled","source","display","audio","switch_device","switch_input"}):keys(o,{"activity_id","zone_id","label","generated_label","label_override","source","display","audio","switch_device","switch_input"}))||!identifier(o,"activity_id",a.activity_id)||!identifier(o,"zone_id",a.zone_id)||!label(o,a.label,a.generated_label,a.label_override)||!optional_id(o,"source",a.source)||!optional_id(o,"display",a.display)||!optional_id(o,"audio",a.audio)||!optional_id(o,"switch_device",a.switch_device)||!number(o,"switch_input",input,255))return false;a.switch_input=static_cast<uint8_t>(input);return true;})||
       !list(root.get(),"zones",c.zones,16,[](cJSON* o,ConfiguredZone& z){return keys(o,{"zone_id","label","generated_label","label_override","activity_order"})&&identifier(o,"zone_id",z.zone_id)&&label(o,z.label,z.generated_label,z.label_override)&&id_list(o,"activity_order",z.activity_order,64);})||
       !list(root.get(),"external_slots",c.external_slots,16,[](cJSON* o,ExternalSlot& s){uint64_t index;if(!keys(o,{"slot","activity_id","tombstone","hidden","alias"})||!number(o,"slot",index,15)||!identifier(o,"activity_id",s.activity_id)||!boolean(o,"tombstone",s.tombstone)||!boolean(o,"hidden",s.hidden)||!text(o,"alias",s.alias))return false;s.slot=static_cast<uint16_t>(index);return true;}))return fail();
    if(field(root.get(),"controller_octopus_id")){model::OctopusId controller;if(!identifier(root.get(),"controller_octopus_id",controller))return fail();c.controller_octopus_id=controller;}
    if(!legacy&&!c.controller_octopus_id)return fail();
    if(field(root.get(),"activity_order")&&!id_list(root.get(),"activity_order",c.activity_order,64))return fail();
    // Old topology approvals, availability flags and executable plans are not
    // imported. Identity and saved intent survive regardless of old enabled.
    if(legacy)migrate_missing_selectors(c);
    c.legacy_encoding=legacy;
    if(!validate_configuration_v2(c,error))return false;
    out=std::move(c);error.clear();return true;
}

std::string serialize_configuration_v2(const ConfigurationV2& input,bool executable) {
    auto c=input;const auto sort_by=[](auto& values,auto key){std::sort(values.begin(),values.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});};
    sort_by(c.components,[](const auto& v){return v.record.component_id;});sort_by(c.activities,[](const auto& v){return v.activity_id;});sort_by(c.zones,[](const auto& v){return v.zone_id;});sort_by(c.external_slots,[](const auto& v){return v.slot;});sort_by(c.octopuses,[](const auto& v){return v.octopus_id;});
    Json root(cJSON_CreateObject(),cJSON_Delete);cJSON_AddNumberToObject(root.get(),"version",3);id(root.get(),"consortium_id",c.consortium_id);
    if(!executable){rev(root.get(),"config_revision",c.config_revision);rev(root.get(),"presentation_revision",c.presentation_revision);}
    id(root.get(),"primary_zone_id",c.primary_zone_id);if(c.controller_octopus_id)id(root.get(),"controller_octopus_id",*c.controller_octopus_id);
    if(!executable&&!c.activity_order.empty())ids(root.get(),"activity_order",c.activity_order);
    const auto labels=[&](cJSON* o,const std::string& value,const std::string& generated,bool overridden){if(!executable){string(o,"label",value);string(o,"generated_label",generated);cJSON_AddBoolToObject(o,"label_override",overridden);}};
    array(root.get(),"octopuses",c.octopuses,[&](const auto& b){auto* o=cJSON_CreateObject();id(o,"octopus_id",b.octopus_id);labels(o,b.label,b.generated_label,b.label_override);cJSON_AddNumberToObject(o,"supported_tentacles",b.supported_tentacles);return o;});
    array(root.get(),"components",c.components,[&](const auto& v){auto* o=cJSON_CreateObject();id(o,"component_id",v.record.component_id);labels(o,v.record.label,v.generated_label,v.label_override);if(!executable)string(o,"kind",v.kind);array(o,"qualified_names",v.record.qualified_names,encode_name);return o;});
    array(root.get(),"activities",c.activities,[&](const auto& a){auto* o=cJSON_CreateObject();id(o,"activity_id",a.activity_id);id(o,"zone_id",a.zone_id);labels(o,a.label,a.generated_label,a.label_override);optional(o,"source",a.source);optional(o,"display",a.display);optional(o,"audio",a.audio);optional(o,"switch_device",a.switch_device);cJSON_AddNumberToObject(o,"switch_input",a.switch_input);return o;});
    array(root.get(),"zones",c.zones,[&](const auto& z){auto* o=cJSON_CreateObject();id(o,"zone_id",z.zone_id);labels(o,z.label,z.generated_label,z.label_override);if(!executable)ids(o,"activity_order",z.activity_order);return o;});
    array(root.get(),"external_slots",c.external_slots,[&](const auto& s){auto* o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"slot",s.slot);id(o,"activity_id",s.activity_id);cJSON_AddBoolToObject(o,"tombstone",s.tombstone);if(!executable){cJSON_AddBoolToObject(o,"hidden",s.hidden);string(o,"alias",s.alias);}return o;});
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
    if(upgraded.legacy_encoding)migrate_missing_selectors(upgraded);

    changed = serialize_configuration_v2(upgraded) != serialize_configuration_v2(config);
    config = std::move(upgraded); error.clear(); return true;
}


bool validate_configuration_v2(const ConfigurationV2& c,std::string& error) {
    const auto fail=[&](const char* reason){error=reason;return false;};
    if(!c.consortium_id||!c.config_revision||!c.presentation_revision||!c.primary_zone_id||c.components.size()>64||c.activities.size()>64||c.zones.empty()||c.zones.size()>16||c.external_slots.size()>16||c.octopuses.empty()||c.octopuses.size()>64)return fail("Configuration identifiers, revisions or capacity invalid");
    std::set<model::OctopusId> owners;std::set<model::ComponentId> components;std::set<model::ActivityId> activities;std::set<model::ZoneId> zones;std::set<std::string> selectors;
    for(const auto& b:c.octopuses)if(!b.octopus_id||!owners.insert(b.octopus_id).second||b.label.size()>128||b.generated_label.size()>128||!b.supported_tentacles||b.supported_tentacles>amity::max_tentacles)return fail("Invalid Amity identity or adapter capacity");
    if(c.controller_octopus_id&&(!*c.controller_octopus_id||!owners.count(*c.controller_octopus_id)))return fail("The original controller must be retained");
    for(const auto& v:c.components){
        if(!v.record.component_id||!components.insert(v.record.component_id).second||v.record.label.size()>128||v.generated_label.size()>128||v.record.qualified_names.size()>1)return fail("Invalid configured component");
        if(v.kind!="source"&&v.kind!="display"&&v.kind!="receiver"&&v.kind!="switch"&&v.kind!="audio"&&v.kind!="placeholder")return fail("Unknown equipment presentation kind");
        if(v.record.qualified_names.empty()){
            const auto& original=v.generated_label.empty()?v.record.label:v.generated_label;
            const bool osd=!original.empty()&&original.size()<=14&&std::none_of(original.begin(),original.end(),[](unsigned char byte){return byte<32||byte==127;});
            if(v.kind!="placeholder"||original.empty()||(osd&&(!c.legacy_encoding||c.controller_octopus_id)))return fail("Equipment requires one scoped OSD selector");
        }
        for(const auto& n:v.record.qualified_names){
            const auto owner=std::find_if(c.octopuses.begin(),c.octopuses.end(),[&](const auto& b){return b.octopus_id==n.port.octopus_id;});
            if(!n.valid()||n.port.consortium_id!=c.consortium_id||owner==c.octopuses.end()||n.port.tentacle_id>=owner->supported_tentacles||!selectors.insert(model::name_json(n)).second)return fail("Duplicate, foreign or invalid equipment selector");
        }
    }
    for(const auto& z:c.zones)if(!z.zone_id||!zones.insert(z.zone_id).second||z.label.size()>128||z.generated_label.size()>128||z.activity_order.size()>64)return fail("Invalid screen group");
    if(!zones.count(c.primary_zone_id))return fail("Primary screen group is missing");
    for(const auto& a:c.activities){if(!a.activity_id||!activities.insert(a.activity_id).second||!zones.count(a.zone_id)||a.label.size()>128||a.generated_label.size()>128)return fail("Invalid activity");for(const auto& ref:{a.source,a.display,a.audio,a.switch_device})if(ref&&!components.count(*ref))return fail("Activity references missing equipment");}
    std::set<model::ActivityId> global;
    if(c.activity_order.size()>64)return fail("Activity order exceeds capacity");
    for(auto id:c.activity_order)if(!activities.count(id)||!global.insert(id).second)return fail("Invalid activity presentation order");
    for(const auto& z:c.zones){std::set<model::ActivityId> order;for(auto id:z.activity_order)if(!order.insert(id).second||std::none_of(c.activities.begin(),c.activities.end(),[&](const auto& a){return a.activity_id==id&&a.zone_id==z.zone_id;}))return fail("Invalid screen activity order");for(const auto& a:c.activities)if(a.zone_id==z.zone_id&&!order.count(a.activity_id))return fail("Every activity must occur in its screen order");}
    std::set<uint16_t> slots;std::set<model::ActivityId> assigned;
    for(const auto& s:c.external_slots)if(s.slot>=16||!s.activity_id||!slots.insert(s.slot).second||!assigned.insert(s.activity_id).second||(!s.tombstone&&!activities.count(s.activity_id))||s.alias.size()>128)return fail("Invalid external selector registry");
    if(serialize_configuration_v2(c).size()>configuration_compatibility_limit)return fail("Configuration exceeds saved payload capacity");
    error.clear();return true;
}
bool reconcile_hardware_profile(ConfigurationV2& configuration, model::OctopusId owner, uint8_t capacity,
    bool& changed, std::string& error) {
    changed = false;
    if (!owner || !capacity || capacity > amity::max_tentacles || !validate_configuration_v2(configuration,error)) {
        if(error.empty())error="Invalid HDMI adapter capacity";
        return false;
    }
    auto candidate=configuration;
    auto board=std::find_if(candidate.octopuses.begin(),candidate.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});
    if(board==candidate.octopuses.end()){error="Hardware profile has no configured Amity device identity";return false;}
    // Detachable adapter availability stays with the live owner. The saved
    // roster records only the hardware profile capacity.
    if(board->supported_tentacles<capacity){board->supported_tentacles=capacity;changed=true;}
    if(!validate_configuration_v2(candidate,error))return false;
    configuration=std::move(candidate);error.clear();return true;
}


} // namespace config_store
