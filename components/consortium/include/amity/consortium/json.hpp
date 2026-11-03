// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amity::consortium {
// Protocol JSON is bounded before parsing. Numbers are unsigned integers only;
// every implemented v1 field uses this subset, never floating point counters.
struct Json {
    enum class Type { null_value, boolean, integer, string, object, array };
    Type type = Type::null_value;
    bool boolean = false;
    uint64_t integer = 0;
    std::string string;
    std::vector<std::pair<std::string, Json>> object;
    std::vector<Json> array;
    const Json *get(std::string_view key) const;
    Json *get(std::string_view key);
};
struct JsonError { size_t offset = 0; const char *reason = ""; };
constexpr size_t max_frame_bytes = 8192;
constexpr uint32_t bulk_frame_flag = uint32_t{1} << 31;
// Rejects duplicate decoded keys, NUL, invalid UTF-8, invalid surrogate pairs,
// trailing content, >12 levels, >256 values, and integer overflow.
bool parse_json(std::string_view bytes, Json &out, JsonError &error, size_t maximum_bytes = max_frame_bytes, size_t maximum_values = 256);
bool closed(const Json &value, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {});
bool json_string(const Json *value, std::string &out, size_t min, size_t max);
bool json_uint(const Json *value, uint64_t &out, uint64_t min, uint64_t max);
bool json_bool(const Json *value, bool &out);

// The length prefix's high bit selects bulk transfer budgets; both classes
// retain the same payload bound. Callers validate class against message kind.
// No callback runs for incomplete frames. Expiry is terminal until reset.
class FrameReader {
public:
    static constexpr uint64_t progress_timeout_ms = 2000;
    static constexpr uint64_t frame_timeout_ms = 10000;
    static constexpr uint64_t bulk_progress_timeout_ms = 30000;
    static constexpr uint64_t bulk_frame_timeout_ms = 60000;
    enum class Result { incomplete, frame, invalid_length, timed_out, failed };
    // Consumes at most one frame, leaving the remaining bytes to the caller.
    Result feed(const uint8_t *bytes, size_t size, size_t &consumed, uint64_t now_ms);
    Result check_deadline(uint64_t now_ms);
    std::string_view body() const { return body_; }
    bool bulk()const{return bulk_;}
    uint64_t progress_timeout()const{return bulk_?bulk_progress_timeout_ms:progress_timeout_ms;}
    uint64_t frame_timeout()const{return bulk_?bulk_frame_timeout_ms:frame_timeout_ms;}
    // Read-only diagnostics; admission/deadline decisions remain in the parser.
    struct Progress {uint64_t idle_ms,total_ms;bool clock_reversed;};
    Progress progress(uint64_t now)const{return {begun_&&now>=last_progress_?now-last_progress_:0,begun_&&now>=started_at_?now-started_at_:0,begun_&&now<last_progress_};}
    void next_frame();
    void reset();
private:
    uint8_t prefix_[4]{};
    size_t prefix_size_ = 0;
    size_t expected_ = 0;
    std::string body_;
    uint64_t last_progress_ = 0, started_at_ = 0;
    bool begun_ = false;
    bool complete_ = false;
    bool failed_ = false;
    bool bulk_ = false;
};
std::vector<uint8_t> frame(std::string_view body,bool bulk=false);
} // namespace amity::consortium
