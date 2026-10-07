#include "swipe_sequence.hpp"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    amity::SwipeSequence sequence;
    std::vector<std::pair<uint8_t,uint32_t>> sent;
    bool admit=true;
    auto send=[&](uint8_t key,uint32_t generation) {
        if(!admit)return false;
        sent.emplace_back(key,generation);
        return true;
    };
    sequence.add(0x04,3,7);
    sequence.tick(249999,7,send);assert(sent.empty());
    sequence.tick(250000,7,send);assert(sent.size()==1&&!sequence.idle());
    sequence.tick(250001,7,send);assert(sent.size()==1);
    admit=false;sequence.tick(500000,7,send);assert(sent.size()==1);
    admit=true;sequence.tick(750000,7,send);sequence.tick(1000000,7,send);
    assert(sent.size()==3&&sequence.idle());
    for(const auto &step:sent)assert(step.first==0x04&&step.second==7);

    // Several detections coalesce per direction, with Python's ten-step limit.
    sequence.add(0x01,6,7);sequence.add(0x01,6,7);sequence.add(0x02,2,7);
    sequence.add(0x03,1,7);sequence.add(0x04,1,7);sequence.add(0x01,6,7);
    for(unsigned i=0;i<20;++i)sequence.tick(1250000+i*250000,7,send);
    assert(sent.size()==17&&sequence.idle());
    for(size_t i=3;i<13;++i)assert(sent[i].first==0x01);
    assert(sent[13].first==0x02&&sent[14].first==0x02);
    assert(sent[15].first==0x03&&sent[16].first==0x04);

    // A transition drops the remaining inertia instead of navigating the new activity.
    sequence.add(0x04,6,7);sequence.tick(7000000,8,send);
    assert(sequence.idle()&&sent.size()==17);
    sequence.add(0x02,6,7);sequence.add(0x03,1,8);sequence.tick(7250000,8,send);
    assert(sequence.idle()&&sent.size()==18);
    assert(sent.back().first==0x03&&sent.back().second==8);
    sequence.add(0x04,6,8);sequence.cancel();sequence.tick(7500000,8,send);
    assert(sequence.idle()&&sent.size()==18);
    sequence.add(0x40,6,8);sequence.add(0x00,6,8);sequence.add(0x04,0,8);
    assert(sequence.idle());
    puts("Finite swipe pacing, coalescing, bounded counts, admission retry and activity cancellation tests passed");
}
