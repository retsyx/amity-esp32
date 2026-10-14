/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "atomic_controller.hpp"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

using T = ble::RemoteCommandType;
using Event = std::pair<T,uint8_t>;
static bool output(ble::RemoteCommand command, void *context) {
    static_cast<std::vector<Event> *>(context)->emplace_back(command.type,static_cast<uint8_t>(command.value));
    return command.type == T::Press || command.type == T::Release;
}
// Observe bindings without admitting activity changes into a real hub.
struct AtomicFixture {
    ble::RemoteInputController policy;
    ble::AtomicInputAdapter adapter;
    explicit AtomicFixture(std::vector<Event> &events) : policy(output,&events), adapter(policy) { policy.synchronize(1,0); }
    void input(uint16_t page,uint16_t usage,bool down,uint64_t now) { adapter.input(page,usage,down,{now,1,0}); }
    void cancel() { policy.cancel(); }
};
int main() {
    std::vector<Event> events;
    AtomicFixture controller(events);
    controller.input(7,0x52,true,100);
    controller.input(7,0x52,true,120100); // Duplicate HID state is not a repeat clock.
    controller.input(7,0x52,true,999999);
    assert((events == std::vector<Event>{{T::Press,1}}));
    controller.input(7,0x52,false,1000000);
    assert((events == std::vector<Event>{{T::Press,1},{T::Release,0}}));

    events.clear();
    controller.input(7,0x3a,true,2000000);
    assert(events.empty()); // An activity hold takes effect on release.
    controller.input(7,0x3a,false,2600001);
    assert((events == std::vector<Event>{{T::Activity,4}}));
    controller.input(7,0x3d,true,3000000);
    controller.input(7,0x3d,false,3499999);
    assert((events.back() == Event{T::Activity,3}));
    controller.input(7,0x3d,true,4000000);
    controller.input(7,0x3d,false,4500000);
    assert((events.back() == Event{T::Activity,7}));

    events.clear();
    controller.input(7,0x66,true,5000000);
    controller.input(7,0x66,false,5000010);
    assert((events == std::vector<Event>{{T::PowerShort,0}}));
    controller.input(7,0x66,true,6000000);
    controller.input(7,0x66,false,6500000);
    assert(events.back().first == T::PowerLong);
    controller.input(7,0x66,true,7000000);
    controller.cancel(); // Disconnect must not toggle power.
    controller.input(7,0x66,false,8000000);
    assert(events.size() == 2);

    events.clear();
    controller.input(7,0x80,true,9000000);
    controller.input(0xc,0xe9,true,9000001); // Same CEC key through another report.
    controller.input(7,0x80,false,9000002);
    assert((events == std::vector<Event>{{T::Press,0x41}}));
    controller.input(0xc,0xe9,false,9000003);
    assert(events.back().first == T::Release && events.size() == 2);

    events.clear();
    controller.input(7,0x52,true,10000000); // Up, then volume interrupts, then Up resumes.
    controller.input(7,0x80,true,10000001);
    controller.input(7,0x80,false,10000002);
    controller.input(7,0x52,false,10000003);
    assert((events == std::vector<Event>{{T::Press,1},{T::Release,0},{T::Press,0x41},
                                        {T::Release,0},{T::Press,1},{T::Release,0}}));

    events.clear();
    controller.input(7,0x52,true,11000000);
    controller.input(7,0x3a,true,11000001);
    controller.input(7,0x3a,false,11000002);
    assert((events == std::vector<Event>{{T::Press,1},{T::Release,0},{T::Activity,0}}));
    controller.input(7,0x52,true,12000000); // Held arrow does not leak into the new activity.
    controller.input(7,0x52,false,12000001);
    assert(events.size() == 3);

    events.clear();
    controller.input(7,0x52,true,13000000);
    controller.input(7,0x3a,true,13000001);
    controller.cancel(); // Queue loss cancels held arrow and pending activity.
    controller.input(7,0x52,false,13000002);
    controller.input(7,0x3a,false,13000003);
    assert((events == std::vector<Event>{{T::Press,1},{T::Release,0}}));

    puts("Atomic press/release/long-gesture/cancellation tests passed");
}
