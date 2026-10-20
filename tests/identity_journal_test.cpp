#include "identity_journal.hpp"
#include <cassert>
#include <map>
#include <stdexcept>
#include <iostream>

struct Memory : config_store::IdentityBackend {
    std::map<std::string, std::string> records;
    int operations = 0, cut = 0;
    void step() { if (++operations == cut) throw std::runtime_error("power cut"); }
    config_store::ReadResult read(const char *key, std::string &out) override {
        step(); auto it = records.find(key);
        if (it == records.end()) return config_store::ReadResult::missing;
        out = it->second; return config_store::ReadResult::found;
    }
    bool write(const char *key, const std::string &value) override {
        step(); records[key] = value; step(); return true;
    }
};
int main() {
    using Id = amity::system::OctopusId;
    int generated = 0;
    auto random = [&](uint8_t *data, size_t size) { ++generated; std::fill(data, data + size, uint8_t(generated)); return true; };
    Memory complete; Id first;
    assert(config_store::load_identity(complete, first, random));
    const int operations = complete.operations;
    for (int cut = 1; cut <= operations; ++cut) {
        Memory store; store.cut = cut; Id unpublished;
        try { config_store::load_identity(store, unpublished, random); } catch (const std::runtime_error &) {}
        const auto pending = store.records.find("candidate");
        const std::string retained = pending == store.records.end() ? "" : pending->second;
        store.cut = 0; store.operations = 0;
        Id recovered; assert(config_store::load_identity(store, recovered, random));
        if (!retained.empty()) assert(store.records.at("committed") == retained);
        Id again; assert(config_store::load_identity(store, again, random)); assert(again == recovered);
    }
    auto corrupt = complete; corrupt.records["committed"][0] = '2';
    const int before = generated; Id id;
    assert(!config_store::load_identity(corrupt, id, random)); assert(generated == before);
    corrupt = complete; corrupt.records.erase("candidate");
    assert(!config_store::load_identity(corrupt, id, random)); assert(generated == before);
    Memory invalid; invalid.records["candidate"] = "1:unprovisioned:broken";
    assert(!config_store::load_identity(invalid, id, random)); assert(generated == before);
    std::cout << "PASS identity commit/restart cuts, immutable retry, corruption recovery\n";
}
