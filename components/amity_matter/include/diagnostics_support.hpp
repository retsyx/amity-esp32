#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace amity_diagnostics {

// Keep BDX transfers independent of the live, wrapping log ring. Bounded sessions
// also prevent a slow client from retaining an unbounded number of snapshots.
class LogSessions {
public:
    static constexpr uint16_t invalid = UINT16_MAX;
    uint16_t begin(std::string snapshot) {
        for (auto &session : sessions) {
            if (session.handle != invalid) continue;
            while (next == invalid || find(next)) ++next;
            session.handle = next++;
            session.data = std::move(snapshot);
            session.cursor = 0;
            return session.handle;
        }
        return invalid;
    }
    bool collect(uint16_t handle, void *buffer, size_t &length, bool &finished) {
        auto *session = find(handle);
        if (!session || (!buffer && length)) return false;
        length = std::min(length, session->data.size() - session->cursor);
        if (length) memcpy(buffer, session->data.data() + session->cursor, length);
        session->cursor += length;
        finished = session->cursor == session->data.size();
        return true;
    }
    bool end(uint16_t handle) {
        auto *session = find(handle);
        if (!session) return false;
        *session = {};
        return true;
    }
private:
    struct Session { uint16_t handle = invalid; size_t cursor = 0; std::string data; };
    std::array<Session, 2> sessions{};
    uint16_t next = 0;
    Session *find(uint16_t handle) {
        if (handle == invalid) return nullptr;
        for (auto &session : sessions) if (session.handle == handle) return &session;
        return nullptr;
    }
};

inline uint64_t operational_seconds(uint64_t saved_seconds, uint64_t uptime_seconds) {
    const uint64_t maximum = uint64_t{UINT32_MAX} * 3600;
    if (saved_seconds >= maximum || uptime_seconds >= maximum - saved_seconds) return maximum;
    return saved_seconds + uptime_seconds;
}
inline uint32_t operational_hours(uint64_t saved_seconds, uint64_t uptime_seconds) {
    return static_cast<uint32_t>(operational_seconds(saved_seconds, uptime_seconds) / 3600);
}
inline size_t copy_recent(const std::string &snapshot, void *buffer, size_t capacity) {
    const size_t size = std::min(capacity, snapshot.size());
    if (size) memcpy(buffer, snapshot.data() + snapshot.size() - size, size);
    return size;
}
}
