#include "intent_configuration_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <openssl/sha.h>

namespace fault {
bool track_new = false;
size_t new_calls = 0, fail_new = 0;
size_t json_calls = 0, fail_json = 0;
bool fail_all_json = false;
size_t hash_calls = 0, fail_hash = 0, fail_hash_also = 0;

void* allocate(size_t size) {
    if (track_new && ++new_calls == fail_new) throw std::bad_alloc{};
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void* json_allocate(size_t size) {
    ++json_calls;
    return fail_all_json || json_calls == fail_json ? nullptr : std::malloc(size);
}
void start_new(size_t nth) { new_calls = 0; fail_new = nth; track_new = true; }
size_t stop_new() { track_new = false; return new_calls; }
}

// Firmware disables C++ exceptions and stops on new failure. Throwing here is
// only a host-test reset boundary: discard the store and inspect durable bytes.
void* operator new(size_t size) { return fault::allocate(size); }
void* operator new[](size_t size) { return fault::allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#ifdef __cpp_sized_deallocation
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }
#endif

// Only the production system_configuration.cpp object renames SHA256 to this
// symbol. All successful requests still execute OpenSSL's real implementation.
extern "C" unsigned char* amity_failure_sha256(const unsigned char* data,
    size_t size, unsigned char* digest) {
    ++fault::hash_calls;
    if (fault::hash_calls == fault::fail_hash || fault::hash_calls == fault::fail_hash_also) return nullptr;
    return SHA256(data, size, digest);
}

struct AtomicMemory final : ConfigBackend {
    std::map<std::string, std::string> values;
    size_t writes = 0;
    ReadResult read(const char* key, std::string& value) override {
        const auto found = values.find(key);
        if (found == values.end()) return ReadResult::missing;
        value = found->second;
        return ReadResult::found;
    }
    bool write(const char* key, const std::string& value) override {
        // Allocation failure must not fabricate an empty committed marker via
        // operator[]. A completed backend write atomically replaces its value.
        std::string complete(value);
        const auto found = values.find(key);
        if (found == values.end()) values.emplace(key, std::move(complete));
        else found->second.swap(complete);
        ++writes;
        return true;
    }
    bool erase(const char* key) override { values.erase(key); return true; }
};

struct Scenario {
    AtomicMemory saved;
    Configuration old, candidate;
    std::string old_text, new_text, candidate_text;
    model::ComponentId unused;

    Scenario() {
        auto initial = direct_configuration();
        unused = fixture_equipment(initial, "Unused player", "source", model::Side::back);
        ConfigurationStore first(saved);
        commit(first, initial, {}, old);
        old_text = serialize_configuration(old);
        candidate = old;
        candidate.activities[0].switch_input = 4;
        candidate.activities[0].label = "Movie time";
        for (auto& component : candidate.components) if (component.record.component_id == unused)
            component.record.label = "Unused equipment retained across allocation failure";
        fixture_equipment(candidate, "Spare player", "source", model::Side::back);
        candidate_text = serialize_configuration(candidate);
        AtomicMemory next = saved;
        ConfigurationStore successful(next);
        Configuration active;
        std::string error;
        assert(successful.recover(active, error) == ConfigResult::ok);
        commit(successful, candidate, active.revisions(), active);
        new_text = serialize_configuration(active);
        assert(old_text != new_text);
        assert_unused(old);
        assert_unused(active);
    }

    void assert_unused(const Configuration& configuration) const {
        assert(std::any_of(configuration.components.begin(), configuration.components.end(),
            [&](const auto& component) { return component.record.component_id == unused; }));
        for (const auto& activity : configuration.activities)
            assert(activity.source != unused && activity.display != unused &&
                activity.audio != unused && activity.switch_device != unused);
    }

    std::string reboot(AtomicMemory& memory) const {
        ConfigurationStore restarted(memory);
        Configuration recovered;
        std::string error;
        assert(restarted.recover(recovered, error) == ConfigResult::ok);
        assert_unused(recovered);
        const auto actual = serialize_configuration(recovered);
        assert(actual == old_text || actual == new_text);
        return actual;
    }
};

size_t hash_failures(const Scenario& scenario) {
    size_t count = 0;
    {
        AtomicMemory memory = scenario.saved;
        ConfigurationStore store(memory);
        Configuration active;
        std::string error;
        assert(store.recover(active, error) == ConfigResult::ok);
        fault::hash_calls = 0;
        assert(store.stage(scenario.candidate, active.revisions(), error) == ConfigResult::ok);
        count = fault::hash_calls;
    }
    assert(count >= 6); // Two comparison pairs, payload digest, record digest.
    const auto reject = [&](size_t first, size_t second) {
        AtomicMemory memory = scenario.saved;
        ConfigurationStore store(memory);
        Configuration active;
        std::string error;
        assert(store.recover(active, error) == ConfigResult::ok);
        const auto durable = memory.values;
        memory.writes = 0;
        fault::hash_calls = 0;
        fault::fail_hash = first;
        fault::fail_hash_also = second;
        const auto result = store.stage(scenario.candidate, active.revisions(), error);
        fault::fail_hash = fault::fail_hash_also = 0;
        assert(result == ConfigResult::storage_error && !store.staged());
        assert(memory.writes == 0 && memory.values == durable);
        assert(scenario.reboot(memory) == scenario.old_text);
    };
    for (size_t nth = 1; nth <= count; ++nth) reject(nth, 0);
    reject(1, 2); // Two empty policy digests must never imply equal policies.
    reject(3, 4); // The same invariant applies to presentation revisions.
    return count + 2;
}

size_t cjson_failures(const Scenario& scenario) {
    cJSON_Hooks hooks{fault::json_allocate, std::free};
    cJSON_InitHooks(&hooks);
    {
        AtomicMemory memory = scenario.saved;
        ConfigurationStore store(memory);
        Configuration active;
        std::string error;
        assert(store.recover(active, error) == ConfigResult::ok);
        fault::json_calls = 0;
        fault::fail_all_json = true;
        assert(serialize_configuration(scenario.candidate) == scenario.candidate_text);
        assert(store.stage(scenario.candidate, active.revisions(), error) == ConfigResult::ok);
        assert(store.commit(active, error) == ConfigResult::ok);
        assert(serialize_configuration(active) == scenario.new_text);
        assert(fault::json_calls == 0);
        fault::fail_all_json = false;
        assert(scenario.reboot(memory) == scenario.new_text);
    }
    Configuration decoded;
    std::string error;
    fault::json_calls = 0;
    assert(parse_configuration(scenario.new_text, decoded, error));
    const size_t count = fault::json_calls;
    assert(count > 0);
    for (size_t nth = 1; nth <= count + 1; ++nth) {
        decoded = scenario.old;
        fault::json_calls = 0;
        fault::fail_json = nth;
        const bool success = parse_configuration(scenario.new_text, decoded, error);
        fault::fail_json = 0;
        assert(success == (nth > count));
        assert(serialize_configuration(decoded) == (success ? scenario.new_text : scenario.old_text));
        scenario.assert_unused(decoded);
    }
    cJSON_InitHooks(nullptr);
    return count;
}

size_t serializer_allocations(const Scenario& scenario) {
    std::string output;
    fault::start_new(0);
    output = serialize_configuration(scenario.candidate);
    const auto count = fault::stop_new();
    assert(output == scenario.candidate_text && count > 0);
    for (size_t nth = 1; nth <= count + 1; ++nth) {
        AtomicMemory memory = scenario.saved;
        output = "unchanged";
        bool interrupted = false;
        fault::start_new(nth);
        try { output = serialize_configuration(scenario.candidate); }
        catch (const std::bad_alloc&) { interrupted = true; }
        fault::stop_new();
        assert(interrupted == (nth <= count));
        assert(output == (interrupted ? "unchanged" : scenario.candidate_text));
        assert(scenario.reboot(memory) == scenario.old_text);
    }
    return count;
}

size_t stage_allocations(const Scenario& scenario) {
    size_t count = 0;
    for (size_t nth = 0; nth <= count + 1; ++nth) {
        AtomicMemory memory = scenario.saved;
        ConfigurationStore store(memory);
        Configuration active;
        std::string error;
        assert(store.recover(active, error) == ConfigResult::ok);
        bool interrupted = false;
        fault::start_new(nth);
        try { assert(store.stage(scenario.candidate, active.revisions(), error) == ConfigResult::ok); }
        catch (const std::bad_alloc&) { interrupted = true; }
        const auto calls = fault::stop_new();
        if (!nth) { count = calls; assert(count > 0); }
        else assert(interrupted == (nth <= count));
        // Even a completely staged record has not activated the candidate.
        assert(scenario.reboot(memory) == scenario.old_text);
    }
    return count;
}

size_t commit_allocations(const Scenario& scenario) {
    size_t count = 0, recovered_old = 0, recovered_new = 0;
    for (size_t nth = 0; nth <= count + 1; ++nth) {
        AtomicMemory memory = scenario.saved;
        ConfigurationStore store(memory);
        Configuration active, committed;
        std::string error;
        assert(store.recover(active, error) == ConfigResult::ok);
        assert(store.stage(scenario.candidate, active.revisions(), error) == ConfigResult::ok);
        bool interrupted = false;
        fault::start_new(nth);
        try { assert(store.commit(committed, error) == ConfigResult::ok); }
        catch (const std::bad_alloc&) { interrupted = true; }
        const auto calls = fault::stop_new();
        if (!nth) { count = calls; assert(count > 0); }
        else assert(interrupted == (nth <= count));
        const auto recovered = scenario.reboot(memory);
        if (recovered == scenario.old_text) ++recovered_old;
        else ++recovered_new;
        if (!interrupted) assert(recovered == scenario.new_text);
    }
    assert(recovered_old && recovered_new);
    return count;
}

int main() {
    const Scenario scenario;
    const auto hashes = hash_failures(scenario);
    const auto json = cjson_failures(scenario);
    const auto serializer = serializer_allocations(scenario);
    const auto stage = stage_allocations(scenario);
    const auto activation = commit_allocations(scenario);
    std::cout << "PASS configuration failure atomicity: " << hashes << " digest failures, "
        << json << " parser allocation failures, zero outgoing cJSON allocations, "
        << serializer << "/" << stage << "/" << activation
        << " serializer/stage/commit allocation cuts retain complete saved equipment\n";
}
