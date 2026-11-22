#include "system_configuration.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <charconv>
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
    if (text.size() > configuration_payload_limit || text.find('\0') != std::string::npos) return {nullptr, cJSON_Delete};
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
bool text(cJSON* o, const char* k, std::string& out, size_t limit = 128) {
    auto* v = field(o, k);
    if (!cJSON_IsString(v) || !v->valuestring || std::strlen(v->valuestring) > limit) return false;
    out = v->valuestring;
    for (unsigned char c : out) if (c < 32 || c == 127) return false;
    return true;
}
bool boolean(cJSON* o, const char* k, bool& out) {
    auto* v = field(o, k);
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
    auto* v = field(o, k); if (!v) return false; if (cJSON_IsNull(v)) { out.reset(); return true; }
    T id; if (!identifier(o, k, id)) return false; out = id; return true;
}
template<class T, class F> bool list(cJSON* o, const char* k, std::vector<T>& out, size_t maximum, F decoder) {
    auto* a = field(o, k); if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > static_cast<int>(maximum)) return false;
    out.clear(); for (auto* item = a->child; item; item = item->next) { T value; if (!decoder(item, value)) return false; out.push_back(std::move(value)); }
    return true;
}
bool name(cJSON* o, model::ConsortiumId consortium, model::QualifiedDeviceName& n) {
    if (!keys(o, {"octopus_id", "tentacle_id", "side", "osd_name"})) return false;
    // Port and name are distinct canonical flat objects, never an LA locator.
    uint64_t index; std::string side; n.port.consortium_id = consortium;
    if (!identifier(o, "octopus_id", n.port.octopus_id) ||
        !number(o, "tentacle_id", index, amity::last_tentacle_id) || !text(o, "side", side, 5) || !text(o, "osd_name", n.osd_name, 14)) return false;
    auto s = model::parse_side(side); if (!s) return false; n.port.side = *s; n.port.tentacle_id = static_cast<uint8_t>(index); return n.valid();
}
bool optional_name(cJSON* o, const char* k, model::ConsortiumId consortium, std::optional<model::QualifiedDeviceName>& out) {
    auto* v = field(o, k); if (!v) return false; if (cJSON_IsNull(v)) { out.reset(); return true; }
    model::QualifiedDeviceName n; if (!name(v, consortium, n)) return false; out = n; return true;
}
template<class T> bool id_list(cJSON* o, const char* k, std::vector<T>& values, size_t maximum) {
    return list(o, k, values, maximum, [](cJSON* v, T& out) { if (!cJSON_IsString(v)) return false; auto p = T::parse(v->valuestring); if (!p) return false; out = *p; return true; });
}

enum class Projection { saved, executable, presentation };
// The same emitter writes canonical bytes or counts them for the storage bound.
// Sorted pointer views leave the immutable configuration and its strings alone.
class ConfigurationWriter {
public:
    explicit ConfigurationWriter(std::string* output = nullptr) : output_(output) {}
    size_t size() const { return size_; }
    void raw(std::string_view text) { size_ += text.size(); if(output_)output_->append(text.data(),text.size()); }
    void quoted(std::string_view text) {
        raw("\"");
        for(unsigned char c:text) {
            switch(c) {
            case '"':raw("\\\"");break;
            case '\\':raw("\\\\");break;
            case '\b':raw("\\b");break;
            case '\f':raw("\\f");break;
            case '\n':raw("\\n");break;
            case '\r':raw("\\r");break;
            case '\t':raw("\\t");break;
            default:
                if(c<32){static constexpr char hex[]="0123456789abcdef";char escaped[]={'\\','u','0','0',hex[c>>4],hex[c&15]};raw({escaped,sizeof(escaped)});}
                else{const char byte=char(c);raw({&byte,1});}
            }
        }
        raw("\"");
    }
    void number(uint64_t value) { char text[20];const auto result=std::to_chars(text,text+sizeof(text),value);raw({text,size_t(result.ptr-text)}); }
    template<class T> void identifier(T value) {
        static constexpr char hex[]="0123456789abcdef";char text[34];text[0]=text[33]='"';
        for(size_t i=0;i<value.bytes().size();++i){text[1+i*2]=hex[value.bytes()[i]>>4];text[2+i*2]=hex[value.bytes()[i]&15];}
        raw({text,sizeof(text)});
    }
    void field(const char* key) { if(!first_)raw(",");first_=false;quoted(key);raw(":"); }
    void text(const char* key,std::string_view value){field(key);quoted(value);}
    void boolean(const char* key,bool value){field(key);raw(value?"true":"false");}
    void integer(const char* key,uint64_t value){field(key);number(value);}
    template<class T> void id(const char* key,T value){field(key);identifier(value);}
    template<class T> void optional(const char* key,const std::optional<T>& value){field(key);if(value)identifier(*value);else raw("null");}
    template<class F> void object(F write) { const bool previous=first_;first_=true;raw("{");write();raw("}");first_=previous; }
    template<class T> void ids(const char* key,const std::vector<T>& values) {
        field(key);raw("[");bool first=true;for(const auto& value:values){if(!first)raw(",");first=false;identifier(value);}raw("]");
    }
    template<class T,class Key,class F> void array(const char* key,const std::vector<T>& values,Key sort_key,F write) {
        field(key);raw("[");
        if(output_) {
            std::vector<const T*> order;order.reserve(values.size());for(const auto& value:values)order.push_back(&value);
            std::sort(order.begin(),order.end(),[&](const auto* a,const auto* b){return sort_key(*a)<sort_key(*b);});
            bool first=true;for(const auto* value:order){if(!first)raw(",");first=false;object([&]{write(*value);});}
        } else {
            // Ordering cannot change the encoded size and needs no scratch view.
            bool first=true;for(const auto& value:values){if(!first)raw(",");first=false;object([&]{write(value);});}
        }
        raw("]");
    }
private:
    std::string* output_;
    size_t size_=0;
    bool first_=true;
};
void encode_configuration(ConfigurationWriter& w,const Configuration& c,Projection projection) {
    const bool saved=projection==Projection::saved,executable=projection==Projection::executable;
    const auto label=[&](const std::string& value){if(!executable)w.text("label",value);};
    w.object([&]{
        if(projection!=Projection::presentation){
            w.integer("version",current_configuration_version);w.id("consortium_id",c.consortium_id);
            if(saved){w.field("config_revision");w.raw("\"");w.number(c.config_revision);w.raw("\"");w.field("presentation_revision");w.raw("\"");w.number(c.presentation_revision);w.raw("\"");}
            if(c.controller_octopus_id)w.id("controller_octopus_id",*c.controller_octopus_id);
        }
        if(!executable&&!c.activity_order.empty())w.ids("activity_order",c.activity_order);
        if(projection!=Projection::presentation)w.array("octopuses",c.octopuses,[](const auto& value){return value.octopus_id;},[&](const auto& value){
            w.id("octopus_id",value.octopus_id);w.integer("supported_tentacles",value.supported_tentacles);
        });
        w.array("components",c.components,[](const auto& value){return value.record.component_id;},[&](const auto& value){
            w.id("component_id",value.record.component_id);label(value.record.label);
            if(saved)w.text("kind",value.kind);
            if(projection!=Projection::presentation){
                // The document's consortium scopes every saved selector.
                const auto& selector=value.record.qualified_name;w.field("qualified_name");
                if(selector)w.object([&]{w.id("octopus_id",selector->port.octopus_id);w.integer("tentacle_id",selector->port.tentacle_id);w.text("side",model::side_name(selector->port.side));w.text("osd_name",selector->osd_name);});
                else w.raw("null");
            }
        });
        w.array("activities",c.activities,[](const auto& value){return value.activity_id;},[&](const auto& value){
            w.id("activity_id",value.activity_id);label(value.label);
            if(projection!=Projection::presentation){w.optional("source",value.source);w.optional("display",value.display);w.optional("audio",value.audio);w.optional("switch_device",value.switch_device);w.integer("switch_input",value.switch_input);}
        });
        w.array("external_slots",c.external_slots,[](const auto& value){return value.slot;},[&](const auto& value){
            w.integer("slot",value.slot);w.id("activity_id",value.activity_id);if(projection!=Projection::presentation)w.boolean("tombstone",value.tombstone);if(!executable){w.boolean("hidden",value.hidden);w.text("alias",value.alias);}
        });
    });
}
std::string encoded_configuration(const Configuration& c,Projection projection) {
    std::string text;ConfigurationWriter counter;encode_configuration(counter,c,projection);text.reserve(counter.size());
    ConfigurationWriter writer(&text);encode_configuration(writer,c,projection);return text;
}
}

bool initialize_configuration(model::ConsortiumId consortium, model::OctopusId controller, uint8_t capacity,
    Configuration& out, std::string& error) {
    if (!consortium || !controller || !capacity || capacity > amity::max_tentacles) {
        error = "Invalid initial configuration identity or HDMI adapter capacity"; return false;
    }
    Configuration candidate;
    candidate.consortium_id = consortium; candidate.controller_octopus_id = controller;
    candidate.octopuses.push_back({controller, capacity});
    if (!validate_configuration(candidate, error)) return false;
    out = std::move(candidate); error.clear(); return true;
}

bool parse_configuration(const std::string& input, Configuration& out, std::string& error) {
    auto root=parse(input);Configuration c;uint64_t version=0;
    const auto fail=[&]{error="Invalid saved activity field, selector, identifier or revision";return false;};
    if(!root||!number(root.get(),"version",version,current_configuration_version)||version!=current_configuration_version)return fail();
    if(!keys(root.get(),{"version","consortium_id","config_revision","presentation_revision","controller_octopus_id","activity_order","octopuses","components","activities","external_slots"})||
        !identifier(root.get(),"consortium_id",c.consortium_id)||!revision(root.get(),"config_revision",c.config_revision)||!revision(root.get(),"presentation_revision",c.presentation_revision))return fail();
    if(!list(root.get(),"octopuses",c.octopuses,64,[](cJSON* o,ConfiguredOctopus& b){uint64_t capacity=0;return keys(o,{"octopus_id","supported_tentacles"})&&identifier(o,"octopus_id",b.octopus_id)&&number(o,"supported_tentacles",capacity,amity::max_tentacles)&&(b.supported_tentacles=static_cast<uint8_t>(capacity),true);})||
       !list(root.get(),"components",c.components,64,[&](cJSON* o,ConfiguredComponent& v){return keys(o,{"component_id","label","kind","qualified_name"})&&identifier(o,"component_id",v.record.component_id)&&text(o,"label",v.record.label)&&text(o,"kind",v.kind,32)&&optional_name(o,"qualified_name",c.consortium_id,v.record.qualified_name);})||
       !list(root.get(),"activities",c.activities,max_configured_activities,[&](cJSON* o,ConfiguredActivity& a){uint64_t input=0;if(!keys(o,{"activity_id","label","source","display","audio","switch_device","switch_input"})||!identifier(o,"activity_id",a.activity_id)||!text(o,"label",a.label)||!optional_id(o,"source",a.source)||!optional_id(o,"display",a.display)||!optional_id(o,"audio",a.audio)||!optional_id(o,"switch_device",a.switch_device)||!number(o,"switch_input",input,255))return false;a.switch_input=static_cast<uint8_t>(input);return true;})||
       !list(root.get(),"external_slots",c.external_slots,16,[](cJSON* o,ExternalSlot& s){uint64_t index;if(!keys(o,{"slot","activity_id","tombstone","hidden","alias"})||!number(o,"slot",index,15)||!identifier(o,"activity_id",s.activity_id)||!boolean(o,"tombstone",s.tombstone)||!boolean(o,"hidden",s.hidden)||!text(o,"alias",s.alias))return false;s.slot=static_cast<uint16_t>(index);return true;}))return fail();
    if(field(root.get(),"controller_octopus_id")){model::OctopusId controller;if(!identifier(root.get(),"controller_octopus_id",controller))return fail();c.controller_octopus_id=controller;}
    if(!c.controller_octopus_id)return fail();
    if(field(root.get(),"activity_order")&&!id_list(root.get(),"activity_order",c.activity_order,max_configured_activities))return fail();
    if(!validate_configuration(c,error))return false;
    out=std::move(c);error.clear();return true;
}

std::string serialize_configuration(const Configuration& input,bool executable) {
    return encoded_configuration(input,executable?Projection::executable:Projection::saved);
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
    return amity::system::hex({reinterpret_cast<const char*>(digest), sizeof(digest)});
}
std::string executable_policy_hash(const Configuration& c) {
    const auto encoded=serialize_configuration(c,true);return encoded.empty()?std::string{}:sha256_hex(encoded);
}
std::string presentation_hash(const Configuration& c) {
    const auto encoded=encoded_configuration(c,Projection::presentation);return encoded.empty()?std::string{}:sha256_hex(encoded);
}

const ConfiguredActivity* activity_for_slot(const Configuration& c, unsigned slot) {
    for (const auto& s : c.external_slots) if (s.slot == slot && !s.tombstone) for (const auto& a : c.activities) if (a.activity_id == s.activity_id) return &a;
    return nullptr;
}
std::vector<ExternalInput> external_inputs(const Configuration& c) {
    std::vector<ExternalInput> result;
    for (const auto& s : c.external_slots) {
        if (s.tombstone) continue;
        const auto a = std::find_if(c.activities.begin(), c.activities.end(), [&](const auto& v) { return v.activity_id == s.activity_id; });
        if (a == c.activities.end()) continue;
        result.push_back({s.slot, a->activity_id, s.alias.empty() ? a->label : s.alias, s.hidden});
    }
    std::sort(result.begin(), result.end(), [](const auto& x, const auto& y) { return x.slot < y.slot; });
    return result;
}
bool retains_activity(const Configuration& previous,const Configuration& candidate,model::ActivityId id) {
    const auto find=[&](const Configuration& config)->const ConfiguredActivity* {
        const auto found=std::find_if(config.activities.begin(),config.activities.end(),[&](const auto& a){return a.activity_id==id;});
        return found==config.activities.end()?nullptr:&*found;
    };
    const auto* before=find(previous);const auto* after=find(candidate);
    if(!before||!after||before->label!=after->label||
       before->source!=after->source||before->display!=after->display||before->audio!=after->audio||
       before->switch_device!=after->switch_device||before->switch_input!=after->switch_input)return false;
    for(const auto& selected:{before->source,before->display,before->audio,before->switch_device})if(selected){
        const auto old=std::find_if(previous.components.begin(),previous.components.end(),[&](const auto& c){return c.record.component_id==*selected;});
        const auto next=std::find_if(candidate.components.begin(),candidate.components.end(),[&](const auto& c){return c.record.component_id==*selected;});
        if(old==previous.components.end()||next==candidate.components.end()||old->record.qualified_name!=next->record.qualified_name)return false;
    }
    return true;
}
std::vector<model::ActivityId> ordered_activities(const Configuration& c) {
    std::vector<model::ActivityId> result;
    const auto add=[&](model::ActivityId id){if(std::find(result.begin(),result.end(),id)==result.end()&&std::any_of(c.activities.begin(),c.activities.end(),[&](const auto& a){return a.activity_id==id;}))result.push_back(id);};
    for(const auto& id:c.activity_order)add(id);
    for(const auto& a:c.activities)add(a.activity_id);
    return result;
}
bool reconcile_external_slots(const Configuration& previous, Configuration& candidate, std::string& error) {
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

bool propose_octopus_removal(const Configuration& previous,model::OctopusId owner,
    Configuration& candidate,std::vector<model::ActivityId>& affected,std::string& error) {
    const auto fail=[&](const char* reason){error=reason;return false;};affected.clear();
    if(!validate_configuration(previous,error))return false;
    if(!owner||previous.controller_octopus_id==owner)return fail("The original Amity cannot remove itself");
    std::set<model::ComponentId> equipment;
    for(const auto& component:previous.components)
        if(component.record.qualified_name&&component.record.qualified_name->port.octopus_id==owner)equipment.insert(component.record.component_id);
    std::set<model::ActivityId> removed;
    for(const auto& activity:previous.activities)
        if((activity.source&&equipment.count(*activity.source))||(activity.display&&equipment.count(*activity.display))||
           (activity.audio&&equipment.count(*activity.audio))||(activity.switch_device&&equipment.count(*activity.switch_device)))
            removed.insert(activity.activity_id);
    for(auto id:ordered_activities(previous))if(removed.count(id))affected.push_back(id);
    auto out=previous;
    out.octopuses.erase(std::remove_if(out.octopuses.begin(),out.octopuses.end(),[&](const auto& board){return board.octopus_id==owner;}),out.octopuses.end());
    out.components.erase(std::remove_if(out.components.begin(),out.components.end(),[&](const auto& component){return equipment.count(component.record.component_id);}),out.components.end());
    out.activities.erase(std::remove_if(out.activities.begin(),out.activities.end(),[&](const auto& activity){return removed.count(activity.activity_id);}),out.activities.end());
    const auto prune=[&](auto& order){order.erase(std::remove_if(order.begin(),order.end(),[&](auto id){return removed.count(id);}),order.end());};
    prune(out.activity_order);
    for(auto& slot:out.external_slots)if(removed.count(slot.activity_id)){slot.tombstone=true;slot.hidden=false;slot.alias.clear();}
    if(!validate_configuration(out,error))return false;
    candidate=std::move(out);error.clear();return true;
}

bool validate_configuration(const Configuration& c,std::string& error) {
    const auto fail=[&](const char* reason){error=reason;return false;};
    if(!c.consortium_id||!c.config_revision||!c.presentation_revision||c.components.size()>64||c.activities.size()>max_configured_activities||c.external_slots.size()>16||c.octopuses.empty()||c.octopuses.size()>64)return fail("Configuration identifiers, revisions or capacity invalid");
    const auto valid_label=[](const std::string& value){return value.size()<=128&&std::none_of(value.begin(),value.end(),[](unsigned char byte){return byte<32||byte==127;});};
    std::set<model::OctopusId> owners;std::set<model::ComponentId> components;std::set<model::ActivityId> activities;std::set<std::string> selectors;
    for(const auto& b:c.octopuses)if(!b.octopus_id||!owners.insert(b.octopus_id).second||!b.supported_tentacles||b.supported_tentacles>amity::max_tentacles)return fail("Invalid Amity identity or adapter capacity");
    if(!c.controller_octopus_id||!*c.controller_octopus_id||!owners.count(*c.controller_octopus_id))return fail("The main Amity must be retained");
    for(const auto& v:c.components){
        if(!v.record.component_id||!components.insert(v.record.component_id).second||!valid_label(v.record.label))return fail("Invalid configured component");
        if(v.kind!="source"&&v.kind!="display"&&v.kind!="receiver"&&v.kind!="switch"&&v.kind!="audio"&&v.kind!="placeholder")return fail("Unknown equipment presentation kind");
        if(!v.record.qualified_name){
            const auto& label=v.record.label;
            const bool osd=!label.empty()&&label.size()<=14&&std::none_of(label.begin(),label.end(),[](unsigned char byte){return byte<32||byte==127;});
            if(v.kind!="placeholder"||label.empty()||osd)return fail("Equipment requires one scoped OSD selector");
        }else{
            const auto& n=*v.record.qualified_name;
            const auto owner=std::find_if(c.octopuses.begin(),c.octopuses.end(),[&](const auto& b){return b.octopus_id==n.port.octopus_id;});
            if(!n.valid()||n.port.consortium_id!=c.consortium_id||owner==c.octopuses.end()||n.port.tentacle_id>=owner->supported_tentacles||!selectors.insert(model::name_json(n)).second)return fail("Duplicate, foreign or invalid equipment selector");
        }
    }
    for(const auto& a:c.activities){if(!a.activity_id||!activities.insert(a.activity_id).second||!valid_label(a.label))return fail("Invalid activity");for(const auto& ref:{a.source,a.display,a.audio,a.switch_device})if(ref&&!components.count(*ref))return fail("Activity references missing equipment");}
    std::set<model::ActivityId> global;
    if(c.activity_order.size()>max_configured_activities)return fail("Activity order exceeds capacity");
    for(auto id:c.activity_order)if(!activities.count(id)||!global.insert(id).second)return fail("Invalid activity presentation order");
    std::set<uint16_t> slots;std::set<model::ActivityId> assigned;
    for(const auto& s:c.external_slots)if(s.slot>=16||!s.activity_id||!slots.insert(s.slot).second||!assigned.insert(s.activity_id).second||(!s.tombstone&&!activities.count(s.activity_id))||!valid_label(s.alias))return fail("Invalid external selector registry");
    ConfigurationWriter counter;encode_configuration(counter,c,Projection::saved);
    if(counter.size()>configuration_payload_limit)return fail("Configuration exceeds saved payload capacity");
    error.clear();return true;
}
bool reconcile_hardware_profile(Configuration& configuration, model::OctopusId owner, uint8_t capacity,
    bool& changed, std::string& error) {
    changed = false;
    if (!owner || !capacity || capacity > amity::max_tentacles || !validate_configuration(configuration,error)) {
        if(error.empty())error="Invalid HDMI adapter capacity";
        return false;
    }
    auto board=std::find_if(configuration.octopuses.begin(),configuration.octopuses.end(),[&](const auto& b){return b.octopus_id==owner;});
    if(board==configuration.octopuses.end()){error="Hardware profile has no configured Amity device identity";return false;}
    // Detachable adapter availability stays with the live owner. The saved
    // roster records only the hardware profile capacity.
    if(board->supported_tentacles<capacity){board->supported_tentacles=capacity;changed=true;}
    error.clear();return true;
}


} // namespace config_store
