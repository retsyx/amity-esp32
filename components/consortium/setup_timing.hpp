// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <array>
namespace amity::consortium::platform {
struct SetupTimingEntry {
    const char *stage="",*result="interrupted";
    uint64_t at_ms=0,elapsed_ms=0,call_ms=0,wait_ms=0;
    uint32_t count=0;int code=0;bool finished=false,summed=false;
};
void emit_setup_timing(const char *area,const SetupTimingEntry&,model::OctopusId,model::LinkId,model::MessageId);
// Diagnostic scalars only. Declare before function guards so destruction emits
// after they unlock; at_ms retains the actual stage completion, not log time.
template<size_t Capacity> class BoundedSetupTimings {
public:
    using Clock=uint64_t(*)();
    BoundedSetupTimings(const char *area,Clock clock):area_(area),clock_(clock){}
    ~BoundedSetupTimings(){for(size_t i=0;i<size_;++i){if(!entries_[i].finished)finish(i,false);emit_setup_timing(area_,entries_[i],peer,link,scan);}}
    size_t begin(const char *stage){if(size_==entries_.size())return size_;auto& entry=entries_[size_];entry.stage=stage;entry.at_ms=clock_();return size_++;}
    void finish(size_t index,bool good,uint32_t count=0,uint64_t call=0,uint64_t wait=0,int code=0){
        if(index>=size_)return;
        auto& entry=entries_[index];if(entry.finished)return;
        const auto now=clock_();entry.elapsed_ms=now>=entry.at_ms?now-entry.at_ms:0;entry.at_ms=now;
        entry.result=good?"ok":"failed";entry.count=count;entry.call_ms=call;entry.wait_ms=wait;entry.code=code;entry.finished=true;
    }
    template<class Action> bool step(const char *stage,Action action){const auto index=begin(stage);const bool good=action();finish(index,good);return good;}
    void sum(const char *stage,uint64_t elapsed,uint32_t count,bool good=true){const auto index=begin(stage);finish(index,good,count,elapsed);if(index<size_){entries_[index].elapsed_ms=elapsed;entries_[index].summed=true;}}
    bool done(bool good){for(size_t i=0;i<size_;++i)if(!entries_[i].finished)finish(i,good);return good;}
    model::OctopusId peer;model::LinkId link;model::MessageId scan;
private:
    const char *area_;Clock clock_;std::array<SetupTimingEntry,Capacity> entries_{};size_t size_=0;
};
using SetupTimings=BoundedSetupTimings<16>;
}
