#include "siri_remote.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

using T = ble::SiriCommandType;
using Event = std::tuple<T, uint8_t, uint8_t>;
static void output(ble::SiriCommand command, void *context) {
    static_cast<std::vector<Event> *>(context)->emplace_back(command.type, command.value, command.count);
}
static void buttons(ble::SiriController &controller, uint16_t state, uint64_t now, bool active) {
    const uint8_t data[] = {static_cast<uint8_t>(state), static_cast<uint8_t>(state >> 8)};
    controller.buttons(data, sizeof(data), now, active);
}
static void finger(uint8_t *data, int x, int y, uint8_t pressure, uint8_t id = 0) {
    // Input coordinates use the Python Touch coordinates (center is zero).
    const unsigned packed_x = static_cast<unsigned>(x - 360) & 0xfff;
    const unsigned packed_y = static_cast<unsigned>(y - 360) & 0xfff;
    data[0] = static_cast<uint8_t>(packed_x);
    data[1] = static_cast<uint8_t>((packed_x >> 8) | ((packed_y & 15) << 4));
    data[2] = static_cast<uint8_t>(packed_y >> 4);
    data[3] = data[4] = 0;
    data[5] = pressure;
    data[6] = id ? 0 : 8;
}
static std::array<uint8_t, 11> touch(uint16_t timestamp, int x, int y, uint8_t pressure = 30, uint8_t id = 0) {
    std::array<uint8_t, 11> data{};
    data[0] = 0x32;
    data[1] = static_cast<uint8_t>(timestamp);
    data[2] = static_cast<uint8_t>(timestamp >> 8);
    data[3] = id ? 0x10 : 0x01;
    finger(data.data() + 4, x, y, pressure, id);
    return data;
}
static void sample(ble::SiriController &controller, uint16_t timestamp, int x, int y,
                   uint8_t pressure = 30, bool active = true, uint8_t id = 0) {
    const auto data = touch(timestamp, x, y, pressure, id);
    controller.touches(data.data(), data.size(), active);
}

static void protocol_tests() {
    using G = ble::SiriGeneration;
    const uint8_t gen2[] = {0x4c, 0, 7, 0x0d, 2, 0x14, 3, 0x80};
    const uint8_t gen3[] = {0x4c, 0, 7, 0x0d, 2, 0x15, 3};
    const uint8_t gen1[] = {0x4c, 0, 0x8a, 7, 0x66, 2};
    const uint8_t gen15[] = {0x4c, 0, 0x8a, 7, 0x6d, 2, 0xff};
    assert(ble::siri_classify_manufacturer(gen2, sizeof(gen2)) == G::Gen2);
    assert(ble::siri_classify_manufacturer(gen3, sizeof(gen3)) == G::Gen3);
    assert(ble::siri_classify_manufacturer(gen1, sizeof(gen1)) == G::Unsupported);
    assert(ble::siri_classify_manufacturer(gen15, sizeof(gen15)) == G::Unsupported);
    for (size_t i = 0; i < 7; ++i) assert(ble::siri_classify_manufacturer(gen2, i) == G::None);
    assert(ble::siri_classify_manufacturer(nullptr, 100) == G::None);
    auto wrong = std::array<uint8_t, 7>{0x4c, 0, 7, 0x0d, 2, 0x16, 3};
    assert(ble::siri_classify_manufacturer(wrong.data(), wrong.size()) == G::None);
    uint16_t product = 0xbeef;
    const uint8_t pnp2[] = {2, 0xac, 5, 0x14, 3, 0, 1};
    const uint8_t pnp3[] = {1, 0x4c, 0, 0x15, 3, 0, 1};
    const uint8_t pnp1[] = {2, 0xac, 5, 0x66, 2, 0, 1};
    const uint8_t foreign[] = {2, 0x4c, 0, 0x14, 3, 0, 1};
    assert(ble::siri_parse_pnp(pnp2, 7, product) && product == 0x0314 && ble::siri_supported_product(product));
    assert(ble::siri_parse_pnp(pnp3, 7, product) && product == 0x0315 && ble::siri_supported_product(product));
    assert(ble::siri_parse_pnp(pnp1, 7, product) && product == 0x0266 && !ble::siri_supported_product(product));
    assert(!ble::siri_supported_product(0x026d));
    assert(!ble::siri_supported_product(0x0316));
    assert(!ble::siri_parse_pnp(foreign, 7, product) && product == 0x0266);
    for (size_t i = 0; i < 7; ++i) assert(!ble::siri_parse_pnp(pnp3, i, product));
    assert(!ble::siri_parse_pnp(nullptr, 7, product));
    uint16_t firmware = 0xbeef;
    for (const char *valid : {"0083", "0x0083", " \t+0X83\r\n"}) {
        assert(ble::siri_parse_firmware(reinterpret_cast<const uint8_t *>(valid), std::strlen(valid), firmware));
        assert(firmware == 0x83);
    }
    for (const char *invalid : {"", "0x", "xyz", "83\x01", "10000", "-1", "8 3"}) {
        assert(!ble::siri_parse_firmware(reinterpret_cast<const uint8_t *>(invalid), std::strlen(invalid), firmware));
        assert(firmware == 0x83);
    }
    assert(!ble::siri_parse_firmware(nullptr, 10, firmware));
    const auto old = ble::siri_profile(0x82), modern = ble::siri_profile(0x83);
    assert(old.input_handle == 0x39 && old.touch_handle == 0x3d && old.audio_handle == 0x35 && old.init_handle == 0x4d);
    assert(old.notification_count == 3 && old.notification_handles[0] == 0x3a && old.notification_handles[1] == 0x3e && old.notification_handles[2] == 0x36);
    assert(old.init_count == 1 && old.init_values[0][0] == 0xf0 && old.init_values[0][1] == 0);
    assert(modern.input_handle == 0x3a && modern.touch_handle == 0x3e && modern.audio_handle == 0x36 && modern.init_handle == 0x4e);
    assert(modern.notification_count == 7 && modern.notification_handles[6] == 0x3a);
    assert((modern.notification_handles == std::array<uint16_t, 7>{0x37, 0x3b, 0x3f, 0x43, 0x47, 0x4b, 0x3a}));
    assert(modern.init_count == 2 && modern.init_values[0][1] == 0xbc && modern.init_values[1][1] == 0xbb);
    for (const auto &profile : {old, modern}) {
        uint8_t state = profile.charging;
        assert(ble::siri_charging_state(profile, &state, 1) == 1);
        state = profile.plugged_in;
        assert(ble::siri_charging_state(profile, &state, 1) == 1);
        state = profile.discharging;
        assert(ble::siri_charging_state(profile, &state, 1) == 0);
        state = 0;
        assert(ble::siri_charging_state(profile, &state, 1) == -1);
        assert(ble::siri_charging_state(profile, &state, 0) == -1);
        assert(ble::siri_charging_state(profile, nullptr, 1) == -1);
    }
}

static void button_tests() {
    std::vector<Event> events;
    ble::SiriController controller(output, &events);
    constexpr uint16_t selectors[] = {8, 0x200, 0x400, 0x800, 0x1000};
    for (uint8_t i = 0; i < 5; ++i) {
        const uint64_t start = i * 2000000;
        buttons(controller, selectors[i], start, false);
        assert(events.size() == i * 2u);
        buttons(controller, selectors[i], start + 400000, false); // Duplicate full-state report.
        buttons(controller, 0, start + 499999, false);
        assert((events.back() == Event{T::Activity, i, 0}));
        buttons(controller, selectors[i], start + 1000000, false);
        buttons(controller, 0, start + 1500000, false);
        assert((events.back() == Event{T::Activity, static_cast<uint8_t>(i + 5), 0}));
    }
    events.clear();
    constexpr uint16_t wire[] = {1, 2, 4, 8, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000};
    constexpr uint8_t cec[] = {9, 0x41, 0x42, 0, 0x35, 0x0d, 0x43, 0x61, 1, 4, 2, 3};
    for (size_t i = 0; i < sizeof(cec); ++i) {
        buttons(controller, wire[i], 20000000, true);
        buttons(controller, wire[i], 22000000, true);
        buttons(controller, 0, 22500000, true);
        assert((events[events.size() - 2] == Event{T::Press, cec[i], 0}));
        assert((events.back() == Event{T::Release, 0, 0}));
    }
    events.clear();
    buttons(controller, 0x200, 0, true);
    buttons(controller, 0x202, 1, true);
    buttons(controller, 0x200, 2, true);
    buttons(controller, 0, 3, true);
    assert((events == std::vector<Event>{{T::Press, 1, 0}, {T::Release, 0, 0}, {T::Press, 0x41, 0},
                                        {T::Release, 0, 0}, {T::Press, 1, 0}, {T::Release, 0, 0}}));
    events.clear();
    buttons(controller, 0x10, 0, true);
    buttons(controller, 0, 499999, true);
    assert((events.back() == Event{T::PowerShort, 0, 0}));
    buttons(controller, 0x10, 1000000, true);
    buttons(controller, 0, 1500000, true);
    assert((events.back() == Event{T::PowerLong, 0, 0}));
    buttons(controller, 0x10, 2000000, false);
    buttons(controller, 0, 2499999, false);
    assert((events.back() == Event{T::Activity, 0, 0}));
    buttons(controller, 0x10, 3000000, false);
    buttons(controller, 0, 3500000, false);
    assert((events.back() == Event{T::PowerLong, 0, 0}));

    for (uint8_t i = 0; i < 5; ++i) {
        events.clear();
        buttons(controller, 0x10, 0, true);
        buttons(controller, 0x10 | selectors[i], 100000, true);
        assert(events.empty());
        buttons(controller, 0x10, 599999, true);
        buttons(controller, 0, 800000, true);
        assert((events == std::vector<Event>{{T::Activity, i, 0}}));
        events.clear();
        buttons(controller, 0x10, 0, true);
        buttons(controller, 0x10 | selectors[i], 100000, true);
        buttons(controller, selectors[i], 600000, true); // Power can be released first.
        buttons(controller, 0, 700000, true);
        assert((events == std::vector<Event>{{T::Activity, static_cast<uint8_t>(i + 5), 0}}));
    }
    events.clear();
    buttons(controller, 0x200, 0, true);
    controller.activity_changed(true); // Switching two active activities.
    buttons(controller, 0x200, 600000, true);
    buttons(controller, 0, 700000, true);
    assert((events == std::vector<Event>{{T::Press, 1, 0}, {T::Release, 0, 0}}));
    events.clear();
    buttons(controller, 8, 0, false);
    controller.activity_changed(true);
    buttons(controller, 8, 600000, true);
    buttons(controller, 0, 700000, true);
    assert(events.empty());
    events.clear();
    buttons(controller, 0x10, 0, true);
    buttons(controller, 0x200, 100000, true); // Newly pressed key shares the power release report.
    buttons(controller, 0, 700000, true);
    assert((events == std::vector<Event>{{T::PowerShort, 0, 0}}));
    events.clear();
    buttons(controller, 8, 0, false);
    buttons(controller, 0x200, 100000, false);
    buttons(controller, 0, 700000, false);
    assert((events == std::vector<Event>{{T::Activity, 0, 0}}));
    events.clear();
    // Main consumes an old queued snapshot with callback emission ignored, then
    // suppresses its held physical mask after discovering the activity changed.
    controller.activity_changed(true);
    buttons(controller, 0x210, 0, true);
    events.clear();
    controller.activity_changed(true);
    buttons(controller, 0x210, 600000, true);
    buttons(controller, 0, 700000, true);
    assert(events.empty());
    events.clear();
    buttons(controller, 0x10, 0, true);
    controller.cancel();
    buttons(controller, 0x10, 600000, true); // Held state after overflow cannot rearm power.
    buttons(controller, 0, 700000, true);
    assert(events.empty());
    buttons(controller, 0x10, 800000, true);
    buttons(controller, 0, 900000, true);
    assert((events == std::vector<Event>{{T::PowerShort, 0, 0}}));
    events.clear();
    buttons(controller, 0x200, 0, true);
    controller.cancel();
    buttons(controller, 0x200, 600000, true);
    buttons(controller, 0, 700000, true);
    assert((events == std::vector<Event>{{T::Press, 1, 0}, {T::Release, 0, 0}}));
    events.clear();
    const uint8_t oversized[] = {8, 0, 0, 0, 0};
    controller.buttons(nullptr, 2, 0, false);
    controller.buttons(oversized, 0, 0, false);
    controller.buttons(oversized, sizeof(oversized), 0, false);
    buttons(controller, 0, 1000000, false);
    assert(events.empty());
}

static void swipe_tests() {
    std::vector<Event> events;
    ble::SiriController controller(output, &events);
    sample(controller, 100, 0, 0, 19); // Hover/light contact is ignored.
    sample(controller, 200, 500, 0, 19);
    assert(events.empty());
    sample(controller, 300, 0, 0, 20);
    sample(controller, 400, 309, 0); // 6.992mm is below the 7mm threshold.
    assert(events.empty());
    sample(controller, 500, 310, 0);
    assert((events == std::vector<Event>{{T::Swipe, 4, 1}}));
    sample(controller, 600, 610, 700); // Axis stays horizontal after recognition.
    assert(events.size() == 1);
    sample(controller, 700, 930, 700);
    assert((events.back() == Event{T::Swipe, 4, 2}));
    sample(controller, 2300, 930, 700, 0); // 1s swipe => momentum only 0 repeats.
    assert(events.size() == 2);
    events.clear();
    sample(controller, 3000, 0, 0);
    sample(controller, 3200, -400, 0);
    sample(controller, 3400, -400, 0, 0); // -45.26mm/s => int(-0.585) is zero.
    assert((events == std::vector<Event>{{T::Swipe, 3, 1}}));
    events.clear();
    sample(controller, 4000, 0, 0);
    sample(controller, 4100, 0, 620);
    sample(controller, 4200, 0, 620, 0); // 140.29mm/s => five momentum repeats.
    assert((events == std::vector<Event>{{T::Swipe, 1, 2}, {T::Swipe, 1, 5}}));
    events.clear();
    sample(controller, 5000, 0, 0);
    sample(controller, 5020, 0, -620);
    sample(controller, 5040, 0, -620, 0);
    assert((events == std::vector<Event>{{T::Swipe, 2, 2}, {T::Swipe, 2, 6}}));
    events.clear();
    sample(controller, 65500, 0, 0);
    sample(controller, 64, 620, 0);
    sample(controller, 164, 620, 0, 0); // 200 ticks across uint16 wrap = 100ms.
    assert((events == std::vector<Event>{{T::Swipe, 4, 2}, {T::Swipe, 4, 5}}));
    events.clear();
    sample(controller, 6000, 0, 0);
    sample(controller, 6000, 700, 0, 0); // Zero elapsed uses Python's 31mm/s fallback.
    assert(events.empty());
    sample(controller, 6100, 0, 0);
    sample(controller, 6110, 700, 1200); // Python gives horizontal axis first chance.
    assert((events == std::vector<Event>{{T::Swipe, 4, 2}}));
    sample(controller, 8000, 700, 1200, 0);
    events.clear();
    sample(controller, 9000, -1500, 0);
    sample(controller, 9100, 2000, 0); // Live distance counts are not momentum capped.
    assert((events == std::vector<Event>{{T::Swipe, 4, 11}}));
    sample(controller, 12000, 2000, 0, 0);

    events.clear();
    sample(controller, 13000, 0, 0);
    buttons(controller, 8, 0, true);
    sample(controller, 13020, 600, 0);
    buttons(controller, 0, 1, true);
    sample(controller, 13040, 1200, 0); // Held finger remains blocked after click release.
    sample(controller, 13060, 1200, 0, 0);
    assert((events == std::vector<Event>{{T::Press, 0, 0}, {T::Release, 0, 0}}));
    events.clear();
    sample(controller, 14000, 0, 0, 30, false);
    sample(controller, 14020, 600, 0, 30, false);
    controller.activity_changed(true);
    sample(controller, 14040, 1200, 0);
    sample(controller, 14060, 1200, 0, 0);
    assert(events.empty());
    sample(controller, 15000, 0, 0);
    controller.activity_changed(true);
    sample(controller, 15020, 600, 0);
    sample(controller, 15040, 1200, 0, 0);
    assert(events.empty());
    sample(controller, 16000, 0, 0);
    controller.cancel();
    sample(controller, 16020, 600, 0);
    sample(controller, 16040, 1200, 0, 0);
    assert(events.empty());
    sample(controller, 16100, 0, 0);
    controller.cancel();
    sample(controller, 16120, 600, 0, 30, true, 1); // Original id0 release was lost offline.
    sample(controller, 16140, 600, 0, 0, true, 1);
    assert(events.empty());
    sample(controller, 16200, 0, 0, 30, true, 1);
    sample(controller, 16220, 600, 0, 30, true, 1);
    assert((events == std::vector<Event>{{T::Swipe, 4, 1}}));
    sample(controller, 16500, 600, 0, 0, true, 1);
    events.clear();

    std::array<uint8_t, 18> two{};
    auto first = touch(17000, 0, 0);
    std::copy(first.begin(), first.end(), two.begin());
    finger(two.data() + 11, 400, 400, 30, 1);
    controller.touches(two.data(), two.size(), true);
    // A packet with two pressed fingers cancels detection, including large jumps.
    first = touch(17020, 1000, 0);
    std::copy(first.begin(), first.end(), two.begin());
    controller.touches(two.data(), two.size(), true);
    assert(events.empty());
    sample(controller, 17025, 0, 0);
    sample(controller, 17030, 1000, 0); // Secondary contact remains held but omitted.
    assert(events.empty());
    first = touch(17040, 1000, 0, 0);
    std::copy(first.begin(), first.end(), two.begin());
    finger(two.data() + 11, 400, 400, 0, 1);
    controller.touches(two.data(), two.size(), true);
    sample(controller, 18000, 0, 0);
    sample(controller, 18020, 0, 500);
    assert((events == std::vector<Event>{{T::Swipe, 1, 1}}));
    sample(controller, 20000, 0, 500, 0);

    events.clear();
    auto malformed = touch(22000, 1000, 0);
    for (size_t length = 0; length < malformed.size(); ++length)
        controller.touches(malformed.data(), length, true);
    controller.touches(nullptr, 11, true);
    controller.touches(malformed.data(), 12, true);
    malformed[0] = 0;
    controller.touches(malformed.data(), malformed.size(), true);
    assert(events.empty());
    sample(controller, 22010, 0, 0);
    sample(controller, 22020, -700, -700);
    assert((events == std::vector<Event>{{T::Swipe, 3, 2}}));
}

int main() {
    protocol_tests();
    button_tests();
    swipe_tests();
    puts("Siri Gen2/3 profile, buttons, standby, macro, cancellation and swipe tests passed");
}
