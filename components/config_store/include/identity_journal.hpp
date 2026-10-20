#pragma once
#include "amity/system/model.hpp"
#include <string>

namespace config_store {
enum class ReadResult { found, missing, error };
struct IdentityBackend {
    virtual ~IdentityBackend() = default;
    virtual ReadResult read(const char *key, std::string &value) = 0;
    virtual bool write(const char *key, const std::string &value) = 0;
};

// Immutable origin/singleton identifiers use a candidate and final marker.
// Restart resumes a candidate; corrupt committed state never generates a new
// identity. The marker names the entire small immutable record, so equality
// covers every byte rather than only a revision or pointer to a mutable slot.
template<class Id> bool load_identity(IdentityBackend &store, Id &out,
        const std::function<bool(uint8_t *, size_t)> &random) {
    std::string committed, candidate;
    const auto marker = store.read("committed", committed);
    if (marker == ReadResult::error) return false;
    const auto pending = store.read("candidate", candidate);
    if (pending == ReadResult::error) return false;
    const auto parse = [](const std::string &record) -> std::optional<Id> {
        constexpr std::string_view prefix = "1:unprovisioned:";
        if (record.size() != prefix.size() + 32 || record.compare(0, prefix.size(), prefix)) return {};
        return Id::parse(std::string_view(record).substr(prefix.size()));
    };
    if (marker == ReadResult::found) {
        if (pending != ReadResult::found || candidate != committed) return false;
        const auto id = parse(committed);
        if (!id) return false;
        out = *id; return true;
    }
    if (pending == ReadResult::missing) {
        const auto id = Id::generate(random);
        if (!id) return false;
        candidate = "1:unprovisioned:" + id->str();
        if (!store.write("candidate", candidate)) return false;
    }
    const auto id = parse(candidate);
    if (!id) return false;
    std::string verified;
    if (store.read("candidate", verified) != ReadResult::found || verified != candidate) return false;
    if (!store.write("committed", candidate)) return false;
    if (store.read("committed", verified) != ReadResult::found || verified != candidate) return false;
    out = *id; return true;
}
}
