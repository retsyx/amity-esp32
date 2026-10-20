#pragma once
#include "amity/consortium/wire.hpp"
#include <functional>

namespace amity::consortium {
struct SnapshotManifest {
    model::MessageId transfer_id;
    model::OctopusId owner_octopus_id;
    model::BootId owner_boot_id;
    uint64_t config_revision = 0;
    uint32_t total_length = 0;
    std::string payload_hash, document_kind;
    uint32_t chunk_count() const { return (total_length + 2047) / 2048; }
};
Error parse_snapshot_manifest(const Json&, SnapshotManifest&, std::string& phase, uint32_t& next_index);
std::string snapshot_manifest_json(const SnapshotManifest&, std::string_view phase, uint32_t next_index);
std::string snapshot_chunk_json(const SnapshotManifest&, uint32_t index, std::string_view payload);
class SnapshotTransfer {
public:
    using Hash = std::function<std::string(std::string_view)>;
    explicit SnapshotTransfer(Hash hash) : hash_(std::move(hash)) {}
    Error begin(const SnapshotManifest&, model::OctopusId expected_owner, model::BootId expected_boot,
                uint64_t expected_config_revision, uint64_t receiver_now_ms);
    Error accept(const Json& chunk, uint64_t receiver_now_ms);
    bool complete() const { return complete_; }
    uint32_t next_index() const { return next_; }
    const std::string& payload() const { return payload_; }
    const SnapshotManifest& manifest() const { return manifest_; }
    void reset();
private:
    Hash hash_;
    SnapshotManifest manifest_;
    std::string payload_;
    uint64_t started_ms_ = 0, last_progress_ms_ = 0;
    uint32_t next_ = 0;
    bool begun_ = false, complete_ = false;
};
} // namespace amity::consortium
