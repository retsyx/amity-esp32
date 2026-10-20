#!/usr/bin/env python3
"""Actual AppleFabrics worker/class lifecycle against a shutdown-sensitive SDK."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'components/amity_matter/apple_fabrics.cpp').read_text();start=s.index('class AppleFabrics final');opening=s.index('{',start);end=opening+1;depth=1
while depth:
    if s[end]=='{':depth+=1
    elif s[end]=='}':depth-=1
    end+=1
actual=s[start:end]+';'
fixture=r'''
#include "apple_fabric_cleanup.hpp"
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <utility>
#include <iostream>
using CHIP_ERROR=int;using esp_err_t=int;using nvs_handle_t=unsigned;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1,NVS_READWRITE=1,CHIP_NO_ERROR=0,CHIP_CONFIG_MAX_FABRICS=16;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
bool pending_record=true,shutdown=false,failsafe=false;unsigned deletes=0,writes=0,cancels=0;
int nvs_open(const char*,int,unsigned*h){*h=1;return 0;}int nvs_get_u8(unsigned,const char*,uint8_t*v){*v=pending_record;return 0;}
int nvs_set_u8(unsigned,const char*,uint8_t v){pending_record=v;++writes;return 0;}int nvs_commit(unsigned){return 0;}
namespace chip {
using FabricIndex=uint8_t;
struct Fabric {FabricIndex index;uint16_t vendor;FabricIndex GetFabricIndex()const{return index;}uint16_t GetVendorId()const{return vendor;}};
struct FabricTable {struct Delegate {virtual ~Delegate()=default;virtual void FabricWillBeRemoved(const FabricTable&,FabricIndex){}virtual void OnFabricRemoved(const FabricTable&,FabricIndex){}virtual void OnFabricCommitted(const FabricTable&,FabricIndex){}};
std::vector<Fabric>fabrics{{1,amity_matter::apple_keychain_vendor}};Delegate*delegate=nullptr;
auto begin(){return fabrics.begin();}auto end(){return fabrics.end();}FabricIndex GetPendingNewFabricIndex(){return 0xff;}
const Fabric*FindFabricWithIndex(FabricIndex index)const{for(auto&f:fabrics)if(f.index==index)return &f;return nullptr;}
int AddFabricDelegate(Delegate*d){assert(!delegate);delegate=d;return 0;}void RemoveFabricDelegate(Delegate*d){assert(delegate==d);delegate=nullptr;}
int Delete(FabricIndex index){assert(!shutdown);++deletes;fabrics.erase(std::remove_if(fabrics.begin(),fabrics.end(),[&](const auto&f){return f.index==index;}),fabrics.end());return 0;}};
struct Server{FabricTable table;struct FailSafe{bool IsFailSafeFullyDisarmed(){return !failsafe;}}fail;
static Server&GetInstance(){static Server s;return s;}FabricTable&GetFabricTable(){return table;}FailSafe&GetFailSafeContext(){return fail;}};
namespace System {struct Layer{};namespace Clock{unsigned Seconds32(unsigned n){return n;}}}
namespace DeviceLayer {
std::vector<std::pair<void(*)(intptr_t),intptr_t>>work;
struct Platform {int ScheduleWork(void(*f)(intptr_t),intptr_t arg){work.push_back({f,arg});return 0;}};
Platform&PlatformMgr(){static Platform p;return p;}
struct Timers{using Fn=void(*)(System::Layer*,void*);Fn pending=nullptr;void*context=nullptr;
int StartTimer(unsigned,Fn fn,void*c){assert(!pending);pending=fn;context=c;return 0;}
void CancelTimer(Fn fn,void*c){++cancels;if(pending){assert(pending==fn&&context==c);pending=nullptr;context=nullptr;}}};
Timers&SystemLayer(){static Timers t;return t;}
}
}
namespace amity_matter {
[[maybe_unused]]constexpr const char*tag="fixture";
''' +actual+r'''
}
void run_work(){auto items=std::move(chip::DeviceLayer::work);chip::DeviceLayer::work.clear();for(auto&item:items)item.first(item.second);}
int main(){using namespace amity_matter;
 {AppleFabrics worker;assert(worker.start()==0&&chip::DeviceLayer::work.size()==1);worker.stop();shutdown=true;run_work();assert(deletes==0&&writes==0&&pending_record&&!chip::Server::GetInstance().table.delegate);worker.stop();assert(cancels==1);}
 shutdown=false;failsafe=true;
 {AppleFabrics worker;assert(worker.start()==0);run_work();assert(chip::DeviceLayer::SystemLayer().pending);worker.stop();assert(!chip::DeviceLayer::SystemLayer().pending);shutdown=true;run_work();assert(!deletes&&!writes&&pending_record);}
 shutdown=false;failsafe=false;
 {AppleFabrics worker;assert(worker.start()==0);run_work();assert(deletes==1&&!pending_record&&writes==1);worker.stop();}
 std::cout<<"PASS actual AppleFabrics: queued work fenced, exact retry timer canceled, delegate removed, pending journal retained, normal cleanup preserved\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);cpp=p/'apple.cpp';binary=p/'test';cpp.write_text(fixture)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(root/'components/amity_matter/include'),str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
