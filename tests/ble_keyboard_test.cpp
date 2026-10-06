#include "hid_keyboard.hpp"
#include "atomic_remote.h"
#include <cassert>
#include <cstdio>
#include <tuple>
#include <vector>

using Event = std::tuple<uint16_t, uint16_t, bool>;
static void key(uint16_t page, uint16_t usage, bool pressed, void *context) {
    static_cast<std::vector<Event> *>(context)->emplace_back(page, usage, pressed);
}
// Standard boot-compatible 6KRO layout in report protocol, with an LED output
// in the middle: output fields must not change input offsets.
static constexpr uint8_t boot[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
    0x95,0x01,0x75,0x08,0x81,0x01,
    0x05,0x08,0x19,0x01,0x29,0x05,0x95,0x05,0x75,0x01,0x91,0x02,
    0x95,0x01,0x75,0x03,0x91,0x01,
    0x05,0x07,0x19,0x00,0x29,0x81,0x15,0x00,0x26,0x81,0x00,
    0x75,0x08,0x95,0x06,0x81,0x00,0xc0
};
int main() {
    std::vector<Event> events;
    ble::HidKeyboard keyboard(key, &events);
    assert(keyboard.add_map(0, boot, sizeof(boot)));
    uint8_t report[8] = {0,0,0x3a,0x52,0,0,0,0};
    assert(keyboard.input(0,0,report,sizeof(report)));
    assert((events == std::vector<Event>{{7,0x3a,true},{7,0x52,true}}));
    assert(keyboard.input(0,0,report,sizeof(report)) && events.size() == 2);
    assert(!keyboard.input(0,0,report,2) && events.size() == 2);
    report[2] = 1; // Rollover preserves held keys.
    assert(!keyboard.input(0,0,report,sizeof(report)) && events.size() == 2);
    report[2] = 0;
    assert(keyboard.input(0,0,report,sizeof(report)));
    assert((events.back() == Event{7,0x3a,false}));
    keyboard.release_all();
    assert((events.back() == Event{7,0x52,false}));
    events.clear(); keyboard.clear();

    // ZMK-style NKRO bitmap with a distinct 16-bit consumer array report ID.
    const uint8_t nkro[] = {
        0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,
        0x19,0x00,0x29,0x87,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x88,0x81,0x02,0xc0,
        0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,0x19,0x00,0x2a,0xff,0x03,
        0x15,0x00,0x26,0xff,0x03,0x75,0x10,0x95,0x01,0x81,0x00,0xc0
    };
    assert(keyboard.add_map(0,nkro,sizeof(nkro)));
    uint8_t bitmap[17]{};
    bitmap[0x66 / 8] = 1 << (0x66 % 8);
    bitmap[0x80 / 8] = 1 << (0x80 % 8);
    assert(keyboard.input(0,1,bitmap,sizeof(bitmap)));
    uint8_t volume[] = {0xe9,0};
    assert(keyboard.input(0,2,volume,sizeof(volume)));
    assert((events == std::vector<Event>{{7,0x66,true},{7,0x80,true},{0xc,0xe9,true}}));
    volume[0] = 0;
    assert(keyboard.input(0,2,volume,sizeof(volume)));
    assert(events.size() == 4 && (events.back() == Event{0xc,0xe9,false}));
    keyboard.clear(); assert(events.size() == 6);

    // Multiple maps referencing the same usage must release only after every
    // report has released it. Explicit usages also exercise non-byte alignment.
    const uint8_t compact[] = {
        0x05,0x0c,0x09,0x01,0xa1,0x01,0x75,0x03,0x95,0x01,0x81,0x01,
        0x09,0xe2,0x09,0xe9,0x75,0x01,0x95,0x02,0x81,0x02,0xc0
    };
    assert(keyboard.add_map(0,compact,sizeof(compact)));
    assert(keyboard.add_map(1,compact,sizeof(compact)));
    events.clear(); uint8_t two = 0x18, zero = 0;
    assert(keyboard.input(0,0,&two,1));
    assert(keyboard.input(1,0,&two,1));
    assert(events.size() == 2);
    assert(keyboard.input(0,0,&zero,1) && events.size() == 2);
    assert(keyboard.input(1,0,&zero,1) && events.size() == 4);

    // Truncated/oversized descriptors and stack underflow fail atomically.
    for (size_t len = 0; len < sizeof(boot); ++len) {
        ble::HidKeyboard malformed;
        assert(!malformed.add_map(0,boot,len));
    }
    const uint8_t pop[] = {0xb4};
    assert(!keyboard.add_map(3,pop,sizeof(pop)));
    const uint8_t large[] = {0x75,0x20,0x96,0xff,0xff,0x81,0};
    assert(!keyboard.add_map(3,large,sizeof(large)));

    atomic_action_t action{};
    const uint16_t usage[] = {0x28,0x29,0x4a,0x4f,0x50,0x51,0x52,0x66,0x7f,0x80,0x81};
    const uint8_t cec[] = {0x00,0x0d,0x09,0x04,0x03,0x02,0x01,0x40,0x43,0x41,0x42};
    for (size_t i=0;i<sizeof(cec);++i) {
        assert(atomic_remote_translate(7,usage[i],&action));
        assert(action.type == ATOMIC_CEC_KEY && action.value == cec[i]);
    }
    for (uint16_t i=0;i<4;++i) {
        assert(atomic_remote_translate(7,0x3a+i,&action));
        assert(action.type == ATOMIC_ACTIVITY && action.value == i);
    }
    assert(atomic_remote_translate(0x0c,0xea,&action) && action.value == 0x42);
    assert(!atomic_remote_translate(7,0x04,&action)); // Alphabetic keys unsupported.
    assert(!atomic_remote_translate(7,0x3e,&action)); // F5 is not an activity.
    puts("BLE descriptor and Atomic mapping tests passed");
}
