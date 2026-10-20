#include "amity/consortium/bootstrap_record.hpp"
#include <algorithm>
namespace amity::consortium {
namespace {
bool hex(std::string_view text,size_t size){return text.size()==size&&std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool generation(std::string_view text,uint64_t& value){
    if(text.empty()||(text.size()>1&&text.front()=='0'))return false;
    value=0;
    for(auto c:text){if(c<'0'||c>'9'||value>(UINT64_MAX-uint64_t(c-'0'))/10)return false;value=value*10+uint64_t(c-'0');}return true;
}
}
bool decode_bootstrap_record(std::string_view bytes,BootstrapRecord& out){
    if(bytes.size()>1024)return false;
    BootstrapRecord result;
    if(bytes.substr(0,2)=="2:"){
        const auto end=bytes.find(':',2);if(end==std::string_view::npos||!generation(bytes.substr(2,end-2),result.generation))return false;
        bytes.remove_prefix(end+1);
    }
    if(bytes.size()!=833||bytes[64]!=':'||!hex(bytes.substr(0,64),64)||!hex(bytes.substr(65),768))return false;
    result.salt_hex=bytes.substr(0,64);result.verifier_hex=bytes.substr(65);out=std::move(result);return true;
}
std::string encode_bootstrap_record(const BootstrapRecord& value){
    if(!hex(value.salt_hex,64)||!hex(value.verifier_hex,768))return {};
    return "2:"+std::to_string(value.generation)+":"+value.salt_hex+":"+value.verifier_hex;
}
}
