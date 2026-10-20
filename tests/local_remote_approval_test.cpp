#include "local_remote_approval.hpp"
#include "json_input.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
namespace m=amity::system;
using namespace web_admin;
template<class T>T id(unsigned n){char value[33];std::snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
int main(){
    const auto boot=id<m::BootId>(1);unsigned effects=0;
    LocalRemoteApprovalBook book;
    const auto body=[&](unsigned request,uint64_t generation){return "{\"request_id\":\""+id<m::MessageId>(request).str()+"\",\"expected_boot_id\":\""+boot.str()+"\",\"expected_remote_generation\":\""+m::format_uint64(generation)+"\"}";};
    const auto decode=[](const std::string& body,bool pair,LocalRemoteRequest& request){std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(parse_json_input(body),cJSON_Delete);return parse_local_remote_request(root.get(),pair,request);};
    LocalRemoteRequest request;const auto original=body(2,0);assert(decode(original,true,request));
    const auto admission=book.execute(boot,request,[&]{++effects;return 0;});assert(admission.accepted&&!admission.duplicate&&admission.generation==1&&effects==1);
    assert(book.execute(boot,request,[&]{++effects;return 9;}).duplicate&&effects==1);
    auto altered=request;altered.pair=false;assert(!book.execute(boot,altered,[&]{++effects;return 0;}).accepted&&effects==1);
    altered=request;altered.id=id<m::MessageId>(3);assert(!book.execute(boot,altered,[&]{++effects;return 0;}).accepted&&effects==1);
    const auto reject=[&](const std::string& from,const std::string& to){auto malformed=original;const auto at=malformed.find(from);assert(at!=std::string::npos);malformed.replace(at,from.size(),to);LocalRemoteRequest ignored;assert(!decode(malformed,true,ignored));};
    reject("\"expected_remote_generation\":\"0\"","\"expected_remote_generation\":0");reject("\"expected_remote_generation\":\"0\"","\"expected_remote_generation\":\"00\"");reject("\"expected_boot_id\"","\"old_boot_id\"");reject("\"request_id\":", "\"unknown\":1,\"request_id\":");reject("\"expected_remote_generation\":\"0\"","\"expected_remote_generation\":\"0\",\"expected_remote_generation\":\"0\"");
    reject("\"expected_remote_generation\":\"0\"","\"expected_remote_generation\":\"0\\u0000\"");
    auto missing=original;missing.erase(missing.find(",\"expected_boot_id\""),missing.find(",\"expected_remote_generation\"")-missing.find(",\"expected_boot_id\""));assert(!decode(missing,true,altered));
    auto targeted=body(3,1);targeted.insert(targeted.size()-1,",\"address\":\"01:23:45:67:89:ab\",\"type\":1");assert(decode(targeted,true,altered)&&altered.targeted&&altered.address[5]==0xab);assert(!decode(targeted,false,altered));
    assert(decode(body(4,1),false,altered));auto failed=book.execute(boot,altered,[&]{++effects;return 77;});assert(failed.accepted&&failed.result==77&&failed.generation==2&&effects==2);
    assert(book.execute(boot,altered,[&]{++effects;return 0;}).result==77&&effects==2); // Failed owner admission still consumes consent.
    std::vector<LocalRemoteRequest> old{request,altered};
    for(unsigned n=0;n<100;++n){LocalRemoteRequest next;assert(decode(body(100+n,book.generation()),n%2==0,next));assert(book.execute(boot,next,[&]{++effects;return 0;}).accepted);old.push_back(next);}
    assert(effects==102&&book.generation()==102);
    for(const auto& previous:old){const auto replay=book.execute(boot,previous,[&]{++effects;return 0;});if(replay.accepted)assert(replay.duplicate);}
    assert(effects==102); // Evicted old Pair/Unpair never receives a fresh approval.
    LocalRemoteApprovalBook reboot;assert(!reboot.execute(id<m::BootId>(9),request,[&]{++effects;return 0;}).accepted);
    LocalRemoteRequest current=request;current.boot=id<m::BootId>(9);assert(reboot.execute(current.boot,current,[&]{++effects;return 0;}).accepted&&effects==103);
    std::cout<<"PASS actual local remote request parsing/admission: malformed, missing, stale, changed, failed, duplicate, 100 evicted approvals and reboot cannot repeat BLE effects\n";
}
