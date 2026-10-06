#include "hid_keyboard.hpp"

#include <algorithm>

namespace ble {
namespace {
constexpr size_t kMaxReports = 16, kMaxFields = 64, kMaxUsages = 256;
constexpr uint32_t kMaxBits = 4096;
struct Global {
    uint16_t page = 0;
    uint32_t size = 0, count = 0;
    int32_t logical_min = 0;
    uint8_t id = 0;
};
uint32_t usage_value(uint32_t value, size_t bytes, uint16_t page) {
    return bytes == 4 ? value : (static_cast<uint32_t>(page) << 16) | value;
}
uint32_t bits(const uint8_t *data, size_t offset, size_t size) {
    uint32_t result = 0;
    for (size_t i = 0; i < size; ++i)
        result |= static_cast<uint32_t>((data[(offset + i) / 8] >> ((offset + i) % 8)) & 1) << i;
    return result;
}
bool contains(const std::vector<uint32_t> &v, uint32_t key) {
    return std::find(v.begin(), v.end(), key) != v.end();
}
void add_unique(std::vector<uint32_t> &v, uint32_t key) {
    if ((key & 0xffff) && !contains(v, key)) v.push_back(key);
}
} // namespace

bool HidKeyboard::add_map(uint8_t map_index, const uint8_t *data, size_t length) {
    if (!data || !length || length > 4096) return false;
    for (const auto &report : reports_) if (report.map == map_index) return false;
    std::vector<Report> parsed;
    Global global;
    std::vector<Global> stack;
    std::vector<uint32_t> usages;
    uint32_t usage_min = 0, usage_max = 0;
    unsigned collections = 0;
    for (size_t pos = 0; pos < length;) {
        const uint8_t prefix = data[pos++];
        if (prefix == 0xfe) { // Long items have no supported HID keyboard semantics.
            if (length - pos < 2) return false;
            const size_t count = data[pos];
            pos += 2;
            if (count > length - pos) return false;
            pos += count;
            continue;
        }
        size_t count = prefix & 3;
        if (count == 3) count = 4;
        if (count > length - pos) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < count; ++i) value |= static_cast<uint32_t>(data[pos++]) << (8 * i);
        const uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: if (value > 0xffff) return false; global.page = value; break;
            case 1:
                global.logical_min = count == 1 ? static_cast<int8_t>(value)
                    : count == 2 ? static_cast<int16_t>(value) : static_cast<int32_t>(value);
                break;
            case 7: global.size = value; break;
            case 8: if (!value || value > 255) return false; global.id = value; break;
            case 9: global.count = value; break;
            case 10: if (stack.size() == 8) return false; stack.push_back(global); break;
            case 11: if (stack.empty()) return false; global = stack.back(); stack.pop_back(); break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 0) {
                if (usages.size() == kMaxUsages) return false;
                usages.push_back(usage_value(value, count, global.page));
            } else if (tag == 1) usage_min = usage_value(value, count, global.page);
            else if (tag == 2) usage_max = usage_value(value, count, global.page);
        } else if (type == 0) {
            if (tag == 8) { // Input fields alone advance input report offsets.
                if (!global.size || global.size > 32 || global.count > kMaxBits ||
                    global.size * global.count > kMaxBits) return false;
                auto it = std::find_if(parsed.begin(), parsed.end(), [&](const Report &r) { return r.id == global.id; });
                if (it == parsed.end()) {
                    if (parsed.size() + reports_.size() == kMaxReports) return false;
                    parsed.push_back(Report{map_index, global.id, 0, {}, {}});
                    it = parsed.end() - 1;
                }
                if (it->bits + global.size * global.count > kMaxBits) return false;
                if (!(value & 1)) { // Ignore constant padding, but preserve its bit offset.
                    if (it->fields.size() == kMaxFields) return false;
                    if (usage_max && usage_max < usage_min) return false;
                    it->fields.push_back(Field{it->bits, static_cast<uint16_t>(global.size),
                        static_cast<uint16_t>(global.count), usage_min, usage_max,
                        global.logical_min, static_cast<bool>(value & 2), usages});
                }
                it->bits += global.size * global.count;
            } else if (tag == 10) ++collections;
            else if (tag == 12) { if (!collections) return false; --collections; }
            usages.clear(); usage_min = usage_max = 0;
        }
    }
    if (collections || !stack.empty() || parsed.empty()) return false;
    reports_.insert(reports_.end(), parsed.begin(), parsed.end());
    return true;
}

std::vector<uint32_t> HidKeyboard::pressed() const {
    std::vector<uint32_t> result;
    for (const auto &report : reports_) for (auto key : report.down) add_unique(result, key);
    return result;
}

void HidKeyboard::dispatch(const std::vector<uint32_t> &before, const std::vector<uint32_t> &after) {
    if (!callback_) return;
    for (auto key : before) if (!contains(after, key)) callback_(key >> 16, key & 0xffff, false, context_);
    for (auto key : after) if (!contains(before, key)) callback_(key >> 16, key & 0xffff, true, context_);
}

bool HidKeyboard::input(uint8_t map, uint8_t id, const uint8_t *data, size_t length) {
    auto report = std::find_if(reports_.begin(), reports_.end(), [&](const Report &r) { return r.map == map && r.id == id; });
    if (report == reports_.end() || !data || length < (report->bits + 7U) / 8) return false;
    std::vector<uint32_t> down;
    for (const auto &field : report->fields) {
        for (size_t i = 0; i < field.count; ++i) {
            const uint32_t value = bits(data, field.offset + i * field.size, field.size);
            if (field.variable && !value) continue;
            const int64_t index = field.variable ? static_cast<int64_t>(i)
                                                : static_cast<int64_t>(value) - field.logical_min;
            if (index < 0) continue;
            uint32_t key = 0;
            if (!field.usages.empty()) {
                if (static_cast<size_t>(index) < field.usages.size()) key = field.usages[index];
                else if (field.variable) key = field.usages.back();
            } else if (field.usage_min && field.usage_min + index <= field.usage_max) {
                key = field.usage_min + index;
            }
            // Keyboard ErrorRollOver/PostFail/ErrorUndefined invalidate the whole
            // report. Keep held keys until a valid report or disconnection.
            if ((key >> 16) == 7 && (key & 0xffff) >= 1 && (key & 0xffff) <= 3) return false;
            add_unique(down, key);
            if (down.size() > kMaxUsages) return false;
        }
    }
    const auto before = pressed();
    report->down = std::move(down);
    dispatch(before, pressed());
    return true;
}

void HidKeyboard::release_all() {
    const auto before = pressed();
    for (auto &report : reports_) report.down.clear();
    dispatch(before, {});
}

void HidKeyboard::clear() { release_all(); reports_.clear(); }
} // namespace ble
