// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/settings.hpp"
#include <algorithm>
namespace amity::consortium {
namespace {
bool text(const Json* value,std::initializer_list<std::string_view> choices){return value&&value->type==Json::Type::string&&std::find(choices.begin(),choices.end(),value->string)!=choices.end();}
bool counter(const Json* value){uint64_t n=0;return decimal(value,n)&&n;}
bool pa(const Json* value,bool allow_empty=false){if(allow_empty&&value&&value->type==Json::Type::string&&value->string.empty())return true;if(!value||value->type!=Json::Type::string||value->string.size()!=7)return false;unsigned number=0;bool zero=false;for(size_t i=0;i<7;++i){if(i%2){if(value->string[i]!='.')return false;continue;}char c=value->string[i];unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(digit==16||(zero&&digit))return false;zero=zero||!digit;number=(number<<4)|digit;}
    return number!=0xffff;}
bool nullable_pa(const Json* value){return value&&(value->type==Json::Type::null_value||pa(value));}
std::string quote(std::string_view value){std::string out="\"";constexpr char hex[]="0123456789abcdef";for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<0x20){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=c;}
    return out+'"';}
}
bool is_settings_kind(Kind kind){return kind==Kind::settings_read||kind==Kind::settings_write||kind==Kind::settings_result;}
bool valid_settings_snapshot(const Json& value){
 model::BootId boot;if(!closed(value,{"boot_id","settings_revision","adapters"})||!id(value.get("boot_id"),boot)||!counter(value.get("settings_revision")))return false;
 const auto* rows=value.get("adapters");if(rows->type!=Json::Type::array||rows->array.size()>amity::max_tentacles)return false;unsigned seen=0;
 for(const auto& row:rows->array){uint64_t slot=0;std::string name;bool configured=false;
  if(!closed(row,{"tentacle_id","name","configured","default_front_physical_address","observed_front_physical_address","effective_front_physical_address"})||!json_uint(row.get("tentacle_id"),slot,0,amity::last_tentacle_id)||(seen&(1u<<slot))||!json_string(row.get("name"),name,0,128)||!json_bool(row.get("configured"),configured)||!pa(row.get("default_front_physical_address"))||!nullable_pa(row.get("observed_front_physical_address"))||!nullable_pa(row.get("effective_front_physical_address")))return false;
  seen|=1u<<slot;
 }
    return true;
}
Error parse_device_settings_write(const Json& value,DeviceSettingsWrite& out){
 DeviceSettingsWrite next;if(!closed(value,{"expected_boot_id","expected_settings_revision","changes"})||!id(value.get("expected_boot_id"),next.expected_boot)||!decimal(value.get("expected_settings_revision"),next.expected_revision)||!next.expected_revision)return Error::invalid_message;
 const auto* changes=value.get("changes");if(changes->type!=Json::Type::array||changes->array.empty()||changes->array.size()>amity::max_tentacles)return Error::invalid_message;unsigned seen=0;
 for(const auto& row:changes->array){uint64_t slot=0;AdapterSettingChange change;
  if(!closed(row,{"tentacle_id","default_front_physical_address"})||!json_uint(row.get("tentacle_id"),slot,0,amity::last_tentacle_id)||(seen&(1u<<slot))||!pa(row.get("default_front_physical_address"),true))return Error::invalid_message;
  seen|=1u<<slot;change.tentacle_id=slot;change.default_front_physical_address=row.get("default_front_physical_address")->string;next.changes.push_back(std::move(change));
 }out=std::move(next);return Error::none;
}
std::string device_settings_write_json(const DeviceSettingsWrite& value){
 std::string out="{\"expected_boot_id\":\""+value.expected_boot.str()+"\",\"expected_settings_revision\":\""+std::to_string(value.expected_revision)+"\",\"changes\":[";
 for(size_t index=0;index<value.changes.size();++index){const auto& change=value.changes[index];if(index)out+=",";out+="{\"tentacle_id\":"+std::to_string(change.tentacle_id)+",\"default_front_physical_address\":"+quote(change.default_front_physical_address)+"}";}
    return out+"]}";
}
bool valid_settings_payload(Kind kind,const Json& value){
 model::MessageId request;if(!id(value.get("request_id"),request))return false;
 if(kind==Kind::settings_read)return closed(value,{"request_id"});
 if(kind==Kind::settings_write){DeviceSettingsWrite write;return closed(value,{"request_id","settings"})&&parse_device_settings_write(*value.get("settings"),write)==Error::none;}
 if(kind!=Kind::settings_result||!closed(value,{"request_id","operation","stage","error","snapshot","rebooting"})||!text(value.get("operation"),{"read","write"})||!text(value.get("stage"),{"admitted","applied","failed"})||!text(value.get("error"),{"","INVALID_MESSAGE","STALE_BOOT","CONFLICT","NOT_READY","EXPIRED","STORAGE_FAILURE","QUEUE_FULL"})||value.get("rebooting")->type!=Json::Type::boolean)return false;
 const bool applied=text(value.get("stage"),{"applied"});
 if(applied)return text(value.get("error"),{""})&&valid_settings_snapshot(*value.get("snapshot"))&&!value.get("rebooting")->boolean;
 return value.get("snapshot")->type==Json::Type::null_value&&!value.get("rebooting")->boolean&&(text(value.get("stage"),{"admitted"})?(text(value.get("operation"),{"write"})&&text(value.get("error"),{""})):!text(value.get("error"),{""}));
}
std::string settings_json(const Json& value){
 switch(value.type){case Json::Type::null_value:return "null";case Json::Type::boolean:return value.boolean?"true":"false";case Json::Type::integer:return std::to_string(value.integer);case Json::Type::string:return quote(value.string);case Json::Type::array:{std::string out="[";for(const auto& row:value.array){if(out.size()>1)out+=",";out+=settings_json(row);}
    return out+"]";}case Json::Type::object:{std::string out="{";for(const auto& item:value.object){if(out.size()>1)out+=",";out+=quote(item.first)+":"+settings_json(item.second);}
    return out+"}";}}
    return {};
}
}
