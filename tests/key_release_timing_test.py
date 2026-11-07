#!/usr/bin/env python3
"""Execute release diagnostics against the real input helper and hub worker."""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def body(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


source = r'''
#include "cec_hub_fixture.hpp"
#include "esp_log.h"
#include <cstdarg>
#include <string>
struct Entry { std::string level,text; };
static std::vector<Entry> entries;
static amity::HubService* observed_hub=nullptr;
static bool inside_submit=false;
static void capture(const char* level,const char*,const char* format,...) {
    char text[512];va_list args;va_start(args,format);std::vsnprintf(text,sizeof(text),format,args);va_end(args);
    if(std::string(text).find("Key release stage=")!=0)return;
    assert(!inside_submit&&"Submission logging must follow the call's return");
    if(observed_hub){
        // A logger must be able to inspect published state: release timing
        // cannot emit while holding the hub's snapshot mutex.
        fixture_require_available_mutex=true;
        observed_hub->snapshot();
        fixture_require_available_mutex=false;
    }
    entries.push_back({level,text});
}
#undef ESP_LOGI
#undef ESP_LOGW
#define ESP_LOGI(tag,...) capture("I",tag,__VA_ARGS__)
#define ESP_LOGW(tag,...) capture("W",tag,__VA_ARGS__)
#include "hub_service.cpp"

namespace input_stage {
struct Facade {
    int64_t delay=0;bool admitted=true;unsigned calls=0;uint8_t key=0;
    bool release_remote_key(uint8_t value){
        assert(!inside_submit);inside_submit=true;++calls;key=value;
        fixture_time+=delay;inside_submit=false;return admitted;
    }
}hub;
MAIN_RELEASE_BODY
}

static void expect(size_t index,const char* stage,int64_t elapsed,bool warning) {
    assert(index<entries.size());const auto& entry=entries[index];
    assert(entry.text.find(std::string("stage=")+stage)!=std::string::npos);
    assert(entry.text.find("elapsed_us="+std::to_string(elapsed))!=std::string::npos);
    assert(entry.level==(warning?"W":"I"));
}
static void input_submission() {
    fixture_tick=0;
    for(const auto delay:{int64_t{5000},int64_t{100000},int64_t{100001}}){
        entries.clear();input_stage::hub.delay=delay;input_stage::hub.admitted=delay!=100001;
        const auto calls=input_stage::hub.calls;
        assert(input_stage::submit_remote_release(4,7)==input_stage::hub.admitted);
        assert(input_stage::hub.calls==calls+1&&input_stage::hub.key==4&&entries.size()==1);
        expect(0,"submit",delay,delay>100000);
        assert(entries[0].text.find("key=4 generation=7")!=std::string::npos);
        assert(entries[0].text.find(std::string("accepted=")+(input_stage::hub.admitted?"1":"0"))!=std::string::npos);
    }
}
static void queue_and_owner_handoff() {
    namespace m=amity::system;
    const auto group=*m::ConsortiumId::parse("11111111111111111111111111111111");
    const auto controller=*m::OctopusId::parse("22222222222222222222222222222222");
    const auto member=*m::OctopusId::parse("33333333333333333333333333333333");
    const auto boot=*m::BootId::parse("44444444444444444444444444444444");
    fixture_present=true;fixture_tick=1000;
    amity::HubService hub;observed_hub=&hub;
    hub.set_system_identity(group,controller);hub.set_boot_identity(boot);assert(hub.start(1,2)==ESP_OK);
    int64_t sender_delay=0;bool sender_admitted=true;std::vector<m::OwnedIntent> edges;
    const m::QualifiedDeviceName remote{{group,member,0,m::Side::back},"Remote"};
    assert(hub.configure_scoped({{"Watch","","source","","",0}},{{"source",remote}},[&](const auto& intent){
        if(intent.kind==m::OwnedIntentKind::key_edges){
            edges.push_back(intent);
            if(intent.edge_count==1&&!intent.edges[0].down){fixture_time+=sender_delay;return sender_admitted;}
        }
        return true;
    },controller,1)==ESP_OK);
    run_one_command();fixture_finish_discovery(hub);
    m::OwnedAdapterSnapshot report;report.owner=member;report.boot=boot;report.ready=true;
    report.devices={{m::Side::back,"Remote",4,4,0x2000,0,true}};
    assert(hub.update_owned_snapshot(report));run_idle(1);
    assert(hub.select_activity(0));run_one_command();
    for(unsigned pass=0;pass<96&&hub.activity_setup_in_progress();++pass)run_idle(1);
    assert(!hub.activity_setup_in_progress()&&hub.snapshot().current_activity==0);
    fixture_tick=0;
    for(unsigned pass=0;pass<3;++pass){
        entries.clear();edges.clear();sender_admitted=pass!=2;
        sender_delay=pass==0?100001:pass==1?100000:5000;
        const int64_t queue_delay=pass==0?5000:pass==1?100000:100001;
        assert(hub.press_key(4,true));run_one_command();
        assert(edges.size()==1&&edges[0].edges[0].down&&entries.empty());
        assert(hub.release_key(4));fixture_time+=queue_delay;run_one_command();
        assert(edges.size()==2&&edges[1].edge_count==1&&!edges[1].edges[0].down);
        assert(edges[1].target.port.octopus_id==member&&edges[1].edges[0].key==4);
        assert(entries.size()==2);expect(0,"dequeue",queue_delay,queue_delay>100000);
        expect(1,"owner_send",sender_delay,sender_delay>100000);
        assert(entries[1].text.find("owner="+member.str())!=std::string::npos);
        assert(entries[1].text.find("generation="+std::to_string(edges[1].generation))!=std::string::npos);
        assert(entries[1].text.find(std::string("admitted=")+(sender_admitted?"1":"0"))!=std::string::npos);
        run_idle(4);assert(edges.size()==2&&entries.size()==2);
    }
    // The existing saturation fallback has no queued Command. Its marker
    // must explain the missing dequeue without losing the one remote up.
    entries.clear();edges.clear();sender_admitted=true;sender_delay=0;
    assert(hub.press_key(4,true));run_one_command();
    for(unsigned count=0;count<32;++count)assert(hub.press_key(0xff));
    assert(!hub.press_key(0xff)&&hub.release_key(4));
    assert(entries.size()==1&&entries[0].text=="Key release stage=fallback key=4");
    run_one_command();
    assert(entries.size()==2&&std::count_if(edges.begin(),edges.end(),[](const auto& intent){
        return intent.edge_count==1&&intent.edges[0].key==4&&!intent.edges[0].down;
    })==1);
    expect(1,"owner_send",0,false);
    for(unsigned count=0;count<31;++count)run_one_command();
    run_idle(4);assert(entries.size()==2);
    observed_hub=nullptr;
}
int main(){input_submission();queue_and_owner_handoff();std::puts("Release submit, queue age and owner handoff timing with unlocked logs passed");}
'''.replace("MAIN_RELEASE_BODY", body("main/main.cpp", "bool submit_remote_release("))

with tempfile.TemporaryDirectory(prefix="amity-release-timing-") as directory:
    root = Path(directory)
    cpp = root / "test.cpp"
    cpp.write_text(source)
    binary = root / "test"
    includes = ["tests", "tests/cec_hub_fakes", "components/amity_system/include",
                "components/config_store/include", "components/cec/include",
                "components/amity_hub/include", "components/amity_hub"]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-g", "-fsanitize=address,undefined",
                    *["-I" + str(ROOT / path) for path in includes], str(cpp),
                    str(ROOT / "components/amity_hub/hub.cpp"),
                    str(ROOT / "components/amity_system/model.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
