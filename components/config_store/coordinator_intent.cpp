#include "coordinator_intent.hpp"
namespace config_store {
namespace {
struct Selection {uint64_t generation=0;unsigned slot=0;std::string hash,record;};
ConfigResult select(ConfigBackend& backend,Selection& out,std::string& error){
    for(const auto* key:{"imark0","imark1"}){
        std::string text;auto status=backend.read(key,text);if(status==ReadResult::error)return ConfigResult::storage_error;if(status==ReadResult::missing)continue;
        if(text.substr(0,2)!="1:"){error="Coordinator intent marker is corrupt";return ConfigResult::recovery_required;}
        auto colon=text.find(':',2);auto gen=colon==std::string::npos?std::optional<uint64_t>{}:model::parse_uint64(std::string_view(text).substr(2,colon-2));
        if(!gen||!*gen||text.size()!=colon+67||(text[colon+1]!='0'&&text[colon+1]!='1')||text[colon+2]!=':'){error="Coordinator intent marker is corrupt";return ConfigResult::recovery_required;}
        const auto hash=text.substr(colon+3);for(char c:hash)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return ConfigResult::recovery_required;
        const auto slot=static_cast<unsigned>(text[colon+1]-'0');if(*gen==out.generation&&(slot!=out.slot||hash!=out.hash))return ConfigResult::recovery_required;
        if(*gen>out.generation)out={*gen,slot,hash,{}};
    }
    if(!out.generation)return ConfigResult::missing;
    if(backend.read(out.slot?"intent1":"intent0",out.record)!=ReadResult::found||out.record.size()>2048||sha256_hex(out.record)!=out.hash){error="Committed logical intent record is missing or corrupt";return ConfigResult::recovery_required;}return ConfigResult::ok;
}
bool decode(const Selection& selected,CoordinatorIntent& intent){
    auto rest=std::string_view(selected.record);std::vector<std::string_view> lines;
    while(!rest.empty()){auto end=rest.find('\n');if(end==std::string_view::npos)return false;lines.push_back(rest.substr(0,end));rest.remove_prefix(end+1);if(lines.size()>20)return false;}
    if(lines.size()<4||lines[0]!="INTENT1")return false;
    auto consortium=model::ConsortiumId::parse(lines[1]);auto revision=model::parse_uint64(lines[2]);auto count=model::parse_uint64(lines[3]);
    if(!consortium||!revision||*revision!=selected.generation||!count||*count>16||lines.size()!=4+*count)return false;
    CoordinatorIntent result;result.consortium_id=*consortium;result.revision=*revision;
    for(size_t n=4;n<lines.size();++n){if(lines[n].size()!=65||lines[n][32]!=':')return false;auto zone=model::ZoneId::parse(lines[n].substr(0,32));auto activity=model::ActivityId::parse(lines[n].substr(33));if(!zone||!activity||(!result.last_requested_sessions.empty()&&!(result.last_requested_sessions.back().zone_id<*zone)))return false;result.last_requested_sessions.push_back({*zone,*activity});}
    intent=std::move(result);return true;
}
bool write(ConfigBackend& backend,const char* key,const std::string& value){if(!backend.write(key,value))return false;std::string actual;return backend.read(key,actual)==ReadResult::found&&actual==value;}
}
ConfigResult CoordinatorIntentJournal::recover(CoordinatorIntent& out,std::string& error){Selection selected;auto status=select(backend_,selected,error);if(status!=ConfigResult::ok)return status;if(!decode(selected,out)){error="Logical coordinator intent metadata is corrupt";return ConfigResult::recovery_required;}return ConfigResult::ok;}
ConfigResult CoordinatorIntentJournal::request(model::ConsortiumId consortium,model::ZoneId zone,std::optional<model::ActivityId> activity,std::string& error){
    if(!consortium||!zone||(activity&&!*activity))return ConfigResult::invalid;
    Selection selected;auto status=select(backend_,selected,error);if(status!=ConfigResult::ok&&status!=ConfigResult::missing)return status;
    CoordinatorIntent intent;intent.consortium_id=consortium;if(status==ConfigResult::ok&&(!decode(selected,intent)||intent.consortium_id!=consortium))return ConfigResult::recovery_required;
    auto entry=std::find_if(intent.last_requested_sessions.begin(),intent.last_requested_sessions.end(),[&](const auto& item){return item.zone_id==zone;});
    if(activity){if(entry!=intent.last_requested_sessions.end()){if(entry->activity_id==*activity)return ConfigResult::ok;entry->activity_id=*activity;}else{if(intent.last_requested_sessions.size()>=16)return ConfigResult::capacity;intent.last_requested_sessions.push_back({zone,*activity});}}
    else{if(entry==intent.last_requested_sessions.end())return ConfigResult::ok;intent.last_requested_sessions.erase(entry);}
    if(selected.generation==UINT64_MAX)return ConfigResult::recovery_required;
    std::sort(intent.last_requested_sessions.begin(),intent.last_requested_sessions.end(),[](const auto& a,const auto& b){return a.zone_id<b.zone_id;});++intent.revision;
    std::string record="INTENT1\n"+consortium.str()+"\n"+model::format_uint64(intent.revision)+"\n"+std::to_string(intent.last_requested_sessions.size())+"\n";for(const auto& item:intent.last_requested_sessions)record+=item.zone_id.str()+":"+item.activity_id.str()+"\n";
    const auto slot=status==ConfigResult::missing?0:!selected.slot;if(!write(backend_,slot?"intent1":"intent0",record))return ConfigResult::storage_error;
    return write(backend_,slot?"imark1":"imark0","1:"+model::format_uint64(intent.revision)+":"+std::to_string(slot)+":"+sha256_hex(record))?ConfigResult::ok:ConfigResult::storage_error;
}
}
