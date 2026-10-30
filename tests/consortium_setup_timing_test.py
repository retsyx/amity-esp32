#!/usr/bin/env python3
"""Check real timing collector/emitter ordering, exact timestamps and log bounds."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "setup_timing.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
bool held=false;uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
std::vector<std::string> logs;
template<class... Args> void ESP_LOGI(const char* tag,const char* format,Args... args){
    assert(!held&&std::string(tag)=="setup_timing");
    char line[384];const auto length=snprintf(line,sizeof(line),format,args...);assert(length>=0&&length<384);logs.emplace_back(line);
}
struct Guard{Guard(){held=true;}~Guard(){held=false;}};
namespace amity::consortium::platform {
'''
SUITE=r'''
}
int main(){using namespace amity::consortium::platform;
 static_assert(sizeof(BoundedSetupTimings<1>)<=160&&sizeof(BoundedSetupTimings<5>)<=432&&sizeof(BoundedSetupTimings<7>)<=560&&sizeof(BoundedSetupTimings<12>)<=880,"diagnostic stack footprint grew");
 {BoundedSetupTimings<2> timing("invite",now_ms);const auto first=timing.begin("peer_validation");
  {Guard guard;clock_ms+=11;timing.finish(first,true,2,9,1);assert(logs.empty());}
  clock_ms+=9;timing.sum("verify_callback",7,3);assert(logs.empty());}
 assert(logs.size()==2&&logs[0].find("at_ms=111 elapsed_ms=11 result=ok")!=std::string::npos&&logs[0].find("count=2 call_ms=9 wait_ms=1")!=std::string::npos);
 assert(logs[1].find("at_ms=120 elapsed_ms=7")!=std::string::npos&&logs[1].find("metric=sum")!=std::string::npos);
 logs.clear();{BoundedSetupTimings<1> timing("admission",now_ms);timing.scan=*amity::system::MessageId::parse("00000000000000000000000000000001");timing.begin("selected");{Guard guard;clock_ms+=3;}}
 assert(logs.size()==1&&logs[0].find("result=failed")!=std::string::npos&&logs[0].find("scan=00000000000000000000000000000001")!=std::string::npos);
 SetupTimingEntry maximal;maximal.stage="verify_callback";maximal.result="failed";maximal.at_ms=maximal.elapsed_ms=maximal.call_ms=maximal.wait_ms=UINT64_MAX;maximal.count=UINT32_MAX;maximal.code=std::numeric_limits<int>::min();
 emit_setup_timing("tls_handshake",maximal,{}, {}, {});assert(logs.back().size()<384);
 for(const auto& line:logs){assert(line.find("password")==std::string::npos&&line.find("ssid")==std::string::npos&&line.find("token")==std::string::npos&&line.find("payload")==std::string::npos);}
 std::cout<<"PASS actual setup timing: deferred emission after guards, original completion timestamps, sum/span distinction, bounded scalar stack/log records, failure and scan anchors\n";
}
'''
def main():
    actual=function((ROOT/'components/consortium/tls_link.cpp').read_text(),'void emit_setup_timing(')
    with tempfile.TemporaryDirectory(prefix='amity-setup-timing-') as directory:
        cpp,binary=Path(directory)/'test.cpp',Path(directory)/'test';cpp.write_text(PREFIX+actual+SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',*['-I'+str(ROOT/p) for p in ['components/consortium','components/consortium/include','components/amity_system/include']],str(cpp),'-o',str(binary)],check=True)
        return subprocess.run([str(binary)],check=False).returncode
if __name__=='__main__':raise SystemExit(main())
