#include "amity/activity_settling.hpp"
#include <cassert>
#include <cstdio>
using amity::ActivitySettling;
using amity::BusSide;
static constexpr auto F = BusSide::Front, B = BusSide::Back;
static cec::Frame frame(std::initializer_list<uint8_t> bytes) {
    cec::Frame f; for (auto b : bytes) f.data[f.size++] = b; return f;
}
static void expect(ActivitySettling &s, BusSide side, uint8_t destination, std::initializer_list<uint8_t> data) {
    s.expect(side, destination, data.begin(), data.size());
}
int main() {
    { // No protocol reply is expected for standby: count from the last TX.
        ActivitySettling s(5); s.reserve(-1); assert(s.active(90000000));
        s.start(-1, {{F, 0, 0}, {B, 4, 0x1000}});
        expect(s, F, 0, {0x36}); expect(s, B, 4, {0x36});
        s.finish(100000000); assert(s.active(104999999)); assert(!s.active(105000000));
    }
    { // The last matching response, not the first request/response, starts quiet time.
        ActivitySettling s(5); s.start(0, {{F, 0, 0}, {B, 4, 0x1000}, {B, 5, 0x2000}});
        expect(s, B, 15, {0x86, 0x10, 0}); expect(s, B, 5, {0x71});
        s.finish(10000000);
        s.received(B, frame({0x4f, 0x82, 0x10, 0}), 0, 10500000);
        s.received(B, frame({0x50, 0x7a, 30}), 0, 11500000);
        assert(s.active(16499999)); assert(!s.active(16500000));
    }
    { // A missing reply times out; no indefinite lock from an absent device.
        ActivitySettling s(5); s.start(0, {{B, 4, 0x1000}});
        expect(s, B, 15, {0x86, 0x10, 0}); s.finish(10000000);
        assert(s.active(16999999)); assert(!s.active(17000000));
    }
    { // Same address on the other bus, malformed, wrong recipient/path and
      // unsolicited messages do not satisfy or extend pending replies.
        ActivitySettling s(5); s.start(0, {{B, 4, 0x1000}});
        expect(s, B, 15, {0x86, 0x10, 0}); s.finish(10000000);
        s.received(F, frame({0x4f, 0x82, 0x10, 0}), 0, 10100000);
        s.received(B, frame({0x4f, 0x82, 0x20, 0}), 0, 10200000);
        s.received(B, frame({0x40, 0x82, 0x10, 0}), 0, 10300000);
        s.received(B, frame({0x4f, 0x82}), 0, 10400000);
        s.received(B, frame({0x8f, 0x82, 0x10, 0}), 0, 10500000);
        s.received(B, frame({0x40, 0x90, 0}), 0, 10600000);
        assert(s.active(16999999)); assert(!s.active(17000000));
    }
    { // Feature Abort resolves the matching directed request only.
        ActivitySettling s(5); s.start(0, {{F, 0, 0}}); expect(s, F, 0, {0x71}); s.finish(10000000);
        s.received(F, frame({0x04, 0x00, 0x8f, 0}), 4, 10200000);
        s.received(F, frame({0x0f, 0x00, 0x71, 0}), 4, 10300000);
        s.received(F, frame({0x04, 0x00, 0x71, 0}), 4, 11000000);
        // Duplicates and later ordinary activity traffic cannot reset the timer.
        s.received(F, frame({0x04, 0x7a, 20}), 4, 14000000);
        assert(s.active(15999999)); assert(!s.active(16000000));
    }
    { // Nonparticipants create no expectations; repairs cannot append requests
      // after setup has finished, so their own replies don't prolong the guard.
        ActivitySettling s(5); s.start(0, {{F, 0, 0}}); expect(s, B, 5, {0x71}); s.finish(10000000);
        expect(s, F, 0, {0x71}); s.received(F, frame({0x04, 0x7a, 20}), 4, 14000000);
        assert(!s.active(15000000));
    }
    { ActivitySettling s(2); s.start(1, {}); s.finish(100); assert(s.active(2000099)); assert(!s.active(2000100)); }
    { ActivitySettling s(0); s.reserve(0); assert(!s.active(0)); s.start(0, {{F, 0, 0}}); expect(s, F, 0, {0x71}); s.finish(1); assert(!s.active(1)); }
    { ActivitySettling s(5); s.reserve(0); s.cancel(); assert(!s.active(1)); }
    puts("Activity settling timing, response matching, timeout, repair isolation and disabled/custom delay tests passed");
}
