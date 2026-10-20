#pragma once
#include "amity/system/coordinator.hpp"
#include <cassert>
#include <deque>
#include <set>
// Sole fake CEC FIFO; the product executor stores no outgoing frames.
struct TestWire {
    using Work=amity::system::WireWork;using Outcome=amity::system::WireOutcome;
    std::deque<Work> fifo;std::optional<Work> active;std::set<uint64_t> canceled;std::vector<Work> emitted;
    std::function<void(uint64_t,Outcome,uint64_t,bool)> completion;
    size_t capacity=8,rejected_before_start=0;bool admitting=true,previous_started=false;
    bool submit(const Work* work,size_t count){if(!admitting||!count)return false;size_t used=outstanding();for(size_t n=0;n<count;++n){if(used>=capacity+(work[n].allow_overflow?1u:0u))return false;++used;}for(size_t n=0;n<count;++n)fifo.push_back(work[n]);return true;}
    void bind(amity::system::LocalCecAgent& agent){agent.set_wire_adapter([this](const Work* w,size_t n){return submit(w,n);},[this](uint64_t id){canceled.insert(id);});completion=[&agent](uint64_t id,Outcome o,uint64_t now,bool started){agent.complete_frame(id,o,now,started);};}
    void bind(amity::system::Coordinator& owner,const amity::system::CecPortRef& port){owner.set_wire_adapter(port,[this](const Work* w,size_t n){return submit(w,n);},[this](uint64_t id){canceled.insert(id);});completion=[&owner,port](uint64_t id,Outcome o,uint64_t now,bool started){owner.complete_frame(port,id,o,now,started);};}
    std::optional<Work> take(uint64_t now){if(active)return {};while(!fifo.empty()){auto work=fifo.front();fifo.pop_front();const bool expired=work.expires_at_ms&&now>=work.expires_at_ms;if(canceled.erase(work.work_id)||expired||(work.requires_previous_start&&!previous_started)){++rejected_before_start;previous_started=false;completion(work.work_id,expired?Outcome::expired:Outcome::canceled,now,false);continue;}active=work;emitted.push_back(work);return work;}return {};}
    void complete(Outcome outcome,uint64_t now,bool started=true){assert(active);const auto id=active->work_id;active.reset();previous_started=started;completion(id,outcome,now,started);}
    void complete(bool applied,uint64_t now){complete(applied?Outcome::success:Outcome::line_error,now);}
    size_t outstanding()const{return fifo.size()+(active?1u:0u);}
};
