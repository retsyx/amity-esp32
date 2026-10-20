#include "candidate_network.hpp"
namespace config_store {
namespace {
void wipe(std::string& value){volatile char* p=value.empty()?nullptr:&value[0];for(size_t n=0;n<value.size();++n)p[n]=0;}
struct Secret {std::string text;~Secret(){wipe(text);}};
bool digest(std::string_view value){return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string hex(std::string_view value){constexpr char alphabet[]="0123456789abcdef";std::string out;for(unsigned char byte:value){out+=alphabet[byte>>4];out+=alphabet[byte&15];}return out;}
bool unhex(std::string_view value,std::string& out,size_t maximum,bool binary=false){if(value.size()>maximum*2||value.size()%2)return false;const auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};out.clear();for(size_t n=0;n<value.size();n+=2){const int a=digit(value[n]),b=digit(value[n+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return binary||out.find('\0')==std::string::npos;}
std::string encode(const CandidateNetwork& value){const auto body="NETWORK1\n"+value.join_id.str()+"\n"+value.proposal_digest+"\n"+(value.decided?"1\n":"0\n")+(value.use_candidate?"1\n":"0\n")+hex(value.old_ssid)+"\n"+hex(value.old_password)+"\n"+hex(value.ssid)+"\n"+hex(value.password)+"\n"+hex(value.old_driver_configuration)+"\n0\n";return body+sha256_hex(body)+"\n";}
bool valid(const CandidateNetwork& value){return value.join_id&&digest(value.proposal_digest)&&value.old_index==0&&value.old_driver_configuration.size()<=1024&&value.old_ssid.size()<=32&&value.old_password.size()<=64&&!value.ssid.empty()&&value.ssid.size()<=32&&value.password.size()<=64&&value.old_ssid.find('\0')==std::string::npos&&value.old_password.find('\0')==std::string::npos&&value.ssid.find('\0')==std::string::npos&&value.password.find('\0')==std::string::npos;}
bool write(ConfigBackend& backend,const std::string& text){if(!backend.write("net_candidate",text))return false;Secret actual;return backend.read("net_candidate",actual.text)==ReadResult::found&&actual.text==text;}
}
CandidateNetwork::~CandidateNetwork(){wipe(old_ssid);wipe(old_password);wipe(ssid);wipe(password);wipe(old_driver_configuration);}
ConfigResult CandidateNetworkJournal::recover(CandidateNetwork& out,std::string& error){
    Secret raw;auto status=backend_.read("net_candidate",raw.text);if(status==ReadResult::error)return ConfigResult::storage_error;if(status==ReadResult::missing||raw.text=="NETWORK_DONE\n")return ConfigResult::missing;
    std::vector<std::string_view> values;std::string_view rest=raw.text;while(!rest.empty()&&values.size()<13){const auto at=rest.find('\n');if(at==std::string_view::npos)break;values.push_back(rest.substr(0,at));rest.remove_prefix(at+1);}
    CandidateNetwork value;
    if(!rest.empty()||values.size()!=12||values[0]!="NETWORK1"||values[10]!="0"||(values[3]!="0"&&values[3]!="1")||(values[4]!="0"&&values[4]!="1")){error="Candidate network journal requires recovery";return ConfigResult::recovery_required;}
    const auto checksum_at=raw.text.size()-65;if(!digest(values[11])||sha256_hex(std::string_view(raw.text).substr(0,checksum_at))!=values[11]){error="Candidate network journal checksum failed";return ConfigResult::recovery_required;}
    auto join=model::JoinId::parse(values[1]);if(!join){error="Candidate network scope is invalid";return ConfigResult::recovery_required;}value.join_id=*join;value.proposal_digest=std::string(values[2]);value.decided=values[3]=="1";value.use_candidate=values[4]=="1";
    if(!unhex(values[5],value.old_ssid,32)||!unhex(values[6],value.old_password,64)||!unhex(values[7],value.ssid,32)||!unhex(values[8],value.password,64)||!unhex(values[9],value.old_driver_configuration,1024,true)||!valid(value)){error="Candidate network journal is invalid";return ConfigResult::recovery_required;}
    out=value;return ConfigResult::ok;
}
ConfigResult CandidateNetworkJournal::stage(const CandidateNetwork& value,std::string& error){
    if(!valid(value)||value.decided||!value.use_candidate)return ConfigResult::invalid;
    CandidateNetwork old;auto status=recover(old,error);if(status==ConfigResult::ok)return encode(old)==encode(value)?ConfigResult::ok:ConfigResult::conflict;if(status!=ConfigResult::missing)return status;
    Secret raw;raw.text=encode(value);return write(backend_,raw.text)?ConfigResult::ok:ConfigResult::storage_error;
}
ConfigResult CandidateNetworkJournal::decide(model::JoinId join,std::string_view hash,bool use_candidate,std::string& error){CandidateNetwork value;auto status=recover(value,error);if(status!=ConfigResult::ok)return status;if(value.join_id!=join||value.proposal_digest!=hash)return ConfigResult::conflict;if(value.decided)return value.use_candidate==use_candidate?ConfigResult::ok:ConfigResult::conflict;value.decided=true;value.use_candidate=use_candidate;Secret raw;raw.text=encode(value);return write(backend_,raw.text)?ConfigResult::ok:ConfigResult::storage_error;}
ConfigResult CandidateNetworkJournal::finish(model::JoinId join,std::string_view hash,std::string& error){CandidateNetwork value;auto status=recover(value,error);if(status==ConfigResult::missing)return ConfigResult::ok;if(status!=ConfigResult::ok)return status;if(value.join_id!=join||value.proposal_digest!=hash||!value.decided)return ConfigResult::conflict;return write(backend_,"NETWORK_DONE\n")?ConfigResult::ok:ConfigResult::storage_error;}
ConfigResult CandidateNetworkJournal::abandon(model::JoinId join,std::string_view hash,std::string& error){CandidateNetwork value;auto status=recover(value,error);if(status==ConfigResult::missing)return ConfigResult::ok;if(status!=ConfigResult::ok)return status;if(value.join_id!=join||value.proposal_digest!=hash||value.decided)return ConfigResult::conflict;return write(backend_,"NETWORK_DONE\n")?ConfigResult::ok:ConfigResult::storage_error;}
}
