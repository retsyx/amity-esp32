#include "activity_editor_model.hpp"
#include "include/json_input.hpp"
#include <cstring>
#include <memory>
#include <map>
#include <set>

namespace web_admin::activity_editor {
namespace cs = config_store;
namespace {
template<class T> bool contains(const std::vector<T>& list, const T& value) { return std::find(list.begin(),list.end(),value)!=list.end(); }
std::vector<std::string> roles(const std::string& kind) {
    if(kind=="display")return {"display","audio"};
    if(kind=="source")return {"source"};
    if(kind=="receiver"||kind=="audio")return {"audio","switch"};
    if(kind=="switch")return {"switch"};
    return {};
}
std::string kind(uint8_t type) {
    if(type==0)return "display";
    if(type==1||type==3||type==4)return "source";
    if(type==5)return "receiver";
    if(type==6||type==7)return "switch";
    return "placeholder";
}
std::string location(const cs::ConfigurationV2& config,const OwnerHostnames& names,const m::QualifiedDeviceName& name,bool has_added_amities,const OwnerCapacities& capacities) {
    const auto current=capacities.find(name.port.octopus_id);
    const auto retained=std::find_if(config.octopuses.begin(),config.octopuses.end(),[&](const auto& owner){return owner.octopus_id==name.port.octopus_id;});
    const auto capacity=current!=capacities.end()?current->second:(retained!=config.octopuses.end()?retained->supported_tentacles:0);
    const std::string adapter=capacity==1&&name.port.tentacle_id==0?"":"Adapter "+std::to_string(name.port.tentacle_id+1);
    if(!has_added_amities&&config.controller_octopus_id==name.port.octopus_id)return adapter;
    const auto owner=names.find(name.port.octopus_id);
    const std::string hostname=owner==names.end()||owner->second.empty()?"Name unavailable":owner->second;
    return adapter.empty()?hostname:hostname+" · "+adapter;
}
const m::CecObservation* observed(const std::vector<m::CecObservation>& observations,const m::QualifiedDeviceName& name) {
    const m::CecObservation* match=nullptr;
    for(const auto& o:observations)if(!o.stale&&o.osd_name&&o.endpoint.port==name.port&&*o.osd_name==name.osd_name){
        if(match&&match->endpoint!=o.endpoint)return nullptr;
        match=&o;
    }
    return match;
}
bool dto_keys(const cJSON* value,std::initializer_list<const char*> allowed){
    if(!cJSON_IsObject(value))return false;
    std::set<std::string> seen;
    for(const auto* child=value->child;child;child=child->next){if(!child->string||!seen.insert(child->string).second)return false;bool known=false;for(const auto* key:allowed)known|=!strcmp(key,child->string);if(!known)return false;}return true;
}
std::string dto_text(const cJSON* value,const char* key){const auto* field=cJSON_GetObjectItemCaseSensitive(value,key);return cJSON_IsString(field)&&field->valuestring?field->valuestring:"";}
bool dto_string(const cJSON* value,const char* key,std::string& out,size_t maximum,bool optional=false){
    const auto* field=cJSON_GetObjectItemCaseSensitive(value,key);if(!field&&optional){out.clear();return true;}
    if(!cJSON_IsString(field)||!field->valuestring||strlen(field->valuestring)>maximum)return false;
    out=field->valuestring;for(unsigned char ch:out)if(ch<32||ch==127)return false;return true;
}
}
std::string component_value(std::optional<m::ComponentId> id) { return id?"component:"+id->str():std::string{}; }
bool parse_submission(const std::string& body,const cs::ConfigurationV2& config,Submission& out,std::string& error) {
    const auto fail=[&](const char* message){error=message;return false;};
    if(body.size()>cs::configuration_compatibility_limit)return fail("The activity document is too large");
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(parse_json_input(body),cJSON_Delete);
    if(!root||!dto_keys(root.get(),{"request_id","version","expected_config_revision","expected_presentation_revision","activities","device_choices"}))return fail("Unknown or invalid activity editor field");
    Submission value;const auto request=m::MessageId::parse(dto_text(root.get(),"request_id"));const auto semantic=m::parse_uint64(dto_text(root.get(),"expected_config_revision"));const auto presentation=m::parse_uint64(dto_text(root.get(),"expected_presentation_revision"));
    if(!request||!semantic||!presentation)return fail("Activity requests require canonical identities and revision strings");
    value.request_id=*request;value.expected={*semantic,*presentation};if(value.expected!=config.revisions())return fail("Activities changed. Reload before saving");
    const auto* version=cJSON_GetObjectItemCaseSensitive(root.get(),"version");const auto* rows=cJSON_GetObjectItemCaseSensitive(root.get(),"activities");
    if(!cJSON_IsNumber(version)||version->valuedouble!=1||!cJSON_IsArray(rows)||cJSON_GetArraySize(rows)>16)return fail("Use activity editor version 1 with at most sixteen activities");
    for(const auto* row=rows->child;row;row=row->next){
        Row entry;if(!dto_keys(row,{"activity_id","name","display","source","audio","switch"})||!dto_string(row,"name",entry.name,63)||!dto_string(row,"display",entry.display,80,true)||!dto_string(row,"source",entry.source,80,true)||!dto_string(row,"audio",entry.audio,80,true))return fail("Invalid activity field");
        if(cJSON_GetObjectItemCaseSensitive(row,"activity_id")){entry.activity_id=m::ActivityId::parse(dto_text(row,"activity_id"));if(!entry.activity_id)return fail("Invalid stable activity identity");}
        const auto* sw=cJSON_GetObjectItemCaseSensitive(row,"switch");
        if(sw&&!cJSON_IsNull(sw)){
            const auto* input=cJSON_GetObjectItemCaseSensitive(sw,"input");
            if(!dto_keys(sw,{"device","input"})||!dto_string(sw,"device",entry.switch_device,80)||!cJSON_IsNumber(input)||input->valuedouble!=input->valueint||input->valueint<0||input->valueint>255)return fail("Switch input must be between 0 and 255");
            entry.switch_input=static_cast<uint8_t>(input->valueint);
        }
        value.rows.push_back(std::move(entry));
    }
    out=std::move(value);error.clear();return true;
}
std::vector<Choice> choices(const cs::ConfigurationV2& config,const std::vector<m::CecObservation>& observations,const OwnerHostnames& names,bool has_added_amities,const OwnerCapacities& capacities) {
    std::vector<Choice> out;
    for(const auto& c:config.components){
        Choice choice;choice.value=component_value(c.record.component_id);choice.component_id=c.record.component_id;
        choice.title=!c.record.label.empty()?c.record.label:(!c.generated_label.empty()?c.generated_label:(!c.record.qualified_names.empty()?c.record.qualified_names[0].osd_name:"Not identified yet"));
        choice.roles=roles(c.kind);
        if(!c.record.qualified_names.empty()){
            choice.native_name=c.record.qualified_names[0];choice.location=location(config,names,*choice.native_name,has_added_amities,capacities);
            if(const auto* o=observed(observations,*choice.native_name))choice.observation=*o;
        }else choice.location="Connection needs assignment";
        // Existing unresolved fields remain editable without silently removing
        // their logical identity, even when their former type is unavailable.
        for(const auto& a:config.activities){
            for(const auto& entry:std::vector<std::pair<std::optional<m::ComponentId>,std::string>>{{a.display,"display"},{a.source,"source"},{a.audio,"audio"},{a.switch_device,"switch"}})
                if(entry.first==c.record.component_id&&!contains(choice.roles,entry.second))choice.roles.push_back(entry.second);
        }
        out.push_back(std::move(choice));
    }
    std::set<std::string> seen;
    for(const auto& o:observations){
        if(o.stale||!o.osd_name)continue;
        m::QualifiedDeviceName name{o.endpoint.port,*o.osd_name};if(!name.valid()||!observed(observations,name))continue;
        const auto key=m::name_json(name);
        if(!seen.insert(key).second||std::any_of(config.components.begin(),config.components.end(),[&](const auto& c){return contains(c.record.qualified_names,name);}))continue;
        if(std::none_of(config.octopuses.begin(),config.octopuses.end(),[&](const auto& b){return b.octopus_id==name.port.octopus_id&&name.port.tentacle_id<b.supported_tentacles;}))continue;
        Choice choice;choice.value="native:"+cs::sha256_hex(key);choice.title=name.osd_name;choice.location=location(config,names,name,has_added_amities,capacities);choice.roles=roles(kind(o.device_type));choice.native_name=name;choice.observation=o;
        if(!choice.roles.empty())out.push_back(std::move(choice));
    }
    return out;
}
bool parse_restore_request(const cJSON* root,const cs::ConfigurationV2& current,m::BootId boot,cs::ConfigurationV2& backup,std::string& error){
    if(!boot||!dto_keys(root,{"request_id","expected_config_revision","expected_presentation_revision","expected_boot_id","confirm","previous_controller_isolated","configuration"})||!m::MessageId::parse(dto_text(root,"request_id"))||dto_text(root,"expected_boot_id")!=boot.str()||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"confirm"))||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"previous_controller_isolated"))){error="Review the current main Amity and confirm the previous main is switched off and disconnected before restoring";return false;}
    const auto semantic=m::parse_uint64(dto_text(root,"expected_config_revision"));const auto presentation=m::parse_uint64(dto_text(root,"expected_presentation_revision"));
    if(!semantic||!presentation||cs::RevisionPair{*semantic,*presentation}!=current.revisions()){error="The saved configuration changed. Review it before restoring";return false;}
    const auto* configuration=cJSON_GetObjectItemCaseSensitive(root,"configuration");if(!cJSON_IsObject(configuration)){error="Choose a complete saved configuration";return false;}
    char* encoded=cJSON_PrintUnformatted(configuration);const std::string raw=encoded?encoded:"";cJSON_free(encoded);
    return cs::parse_configuration_v2(raw,backup,error);
}
std::vector<m::ActivityId> projected_activities(const cs::ConfigurationV2& config) {
    auto result=cs::ordered_activities(config);if(result.size()>16)result.resize(16);
    return result;
}
bool edit(const cs::ConfigurationV2& previous,const std::vector<Row>& rows,const std::vector<Choice>& available,
    const std::function<bool(uint8_t*,size_t)>& random,cs::ConfigurationV2& out,std::vector<std::string>& unresolved,std::string& error) {
    const auto fail=[&](const char* reason){error=reason;return false;};
    if(!previous.controller_octopus_id||!cs::validate_configuration_v2(previous,error)||rows.size()>16)return fail("The saved activity configuration is not ready for editing");
    const auto projection=projected_activities(previous);auto candidate=previous;
    std::set<m::ActivityId> retained;std::set<std::string> names;std::vector<m::ActivityId> order;unresolved.clear();
    const auto fresh=[&](auto tag){using Id=decltype(tag);auto value=Id::generate(random);return value.value_or(Id{});};
    const auto selected=[&](const std::string& value,const char* role,std::optional<m::ComponentId>& id)->bool{
        id.reset();if(value.empty())return true;
        const auto choice=std::find_if(available.begin(),available.end(),[&](const auto& c){return c.value==value&&contains(c.roles,std::string(role));});
        if(choice==available.end()){error="Choose identified equipment from this activity field's list";return false;}
        if(choice->component_id){id=choice->component_id;return true;}
        if(!choice->native_name)return fail("The selected equipment has no scoped OSD name");
        for(const auto& c:candidate.components)if(contains(c.record.qualified_names,*choice->native_name)){id=c.record.component_id;return true;}
        cs::ConfiguredComponent c;c.record.component_id=fresh(m::ComponentId{});if(!c.record.component_id)return fail("Equipment identity allocation failed");
        c.record.qualified_names={*choice->native_name};c.record.label=c.generated_label=choice->title;c.kind=choice->observation?kind(choice->observation->device_type):(std::string(role)=="display"?"display":std::string(role)=="source"?"source":"receiver");id=c.record.component_id;candidate.components.push_back(std::move(c));return true;
    };
    for(const auto& row:rows){
        if(row.name.empty()||row.name.size()>63||!names.insert(row.name).second)return fail("Use distinct activity names of at most 63 bytes");
        cs::ConfiguredActivity activity;
        if(row.activity_id){
            if(!contains(projection,*row.activity_id)||!retained.insert(*row.activity_id).second)return fail("An activity identity is unknown or repeated");
            const auto found=std::find_if(previous.activities.begin(),previous.activities.end(),[&](const auto& a){return a.activity_id==*row.activity_id;});if(found==previous.activities.end())return fail("The activity no longer exists");activity=*found;
        }else{
            activity.activity_id=fresh(m::ActivityId{});activity.zone_id=previous.primary_zone_id;
            if(!activity.activity_id||contains(projection,activity.activity_id)||!retained.insert(activity.activity_id).second)return fail("Activity identity allocation failed");
        }
        activity.label=row.name;activity.generated_label=row.name;activity.label_override=true;activity.switch_input=row.switch_input;
        if(!selected(row.display,"display",activity.display)||!selected(row.source,"source",activity.source)||!selected(row.audio,"audio",activity.audio)||!selected(row.switch_device,"switch",activity.switch_device))return false;
        candidate.activities.erase(std::remove_if(candidate.activities.begin(),candidate.activities.end(),[&](const auto& a){return a.activity_id==activity.activity_id;}),candidate.activities.end());candidate.activities.push_back(activity);order.push_back(activity.activity_id);
    }
    const auto removed=[&](m::ActivityId id){return contains(projection,id)&&!retained.count(id);};
    candidate.activities.erase(std::remove_if(candidate.activities.begin(),candidate.activities.end(),[&](const auto& a){return removed(a.activity_id);}),candidate.activities.end());
    candidate.activity_order=order;for(const auto& id:cs::ordered_activities(previous))if(!contains(projection,id)&&!contains(candidate.activity_order,id))candidate.activity_order.push_back(id);
    for(auto& slot:candidate.external_slots)if(removed(slot.activity_id))slot.tombstone=true;
    for(auto& zone:candidate.zones){
        zone.activity_order.erase(std::remove_if(zone.activity_order.begin(),zone.activity_order.end(),[&](const auto& id){return contains(projection,id)||contains(order,id);}),zone.activity_order.end());
        std::vector<m::ActivityId> ordered;for(const auto& id:order)for(const auto& a:candidate.activities)if(a.activity_id==id&&a.zone_id==zone.zone_id)ordered.push_back(id);
        zone.activity_order.insert(zone.activity_order.begin(),ordered.begin(),ordered.end());
    }
    if(!cs::reconcile_external_slots(previous,candidate,error)||!cs::validate_configuration_v2(candidate,error))return false;
    out=std::move(candidate);error.clear();return true;
}
}
