#include "amity/consortium/transfer.hpp"
#include <algorithm>

namespace amity::consortium {
namespace {
bool digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool valid(const SnapshotManifest& m) {
    return m.transfer_id && m.owner_octopus_id && m.owner_boot_id && m.config_revision && m.total_length && m.total_length <= 32768 &&
        digest(m.payload_hash) && (m.document_kind == "configuration" || m.document_kind == "owner_state");
}
int nibble(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }
}
Error parse_snapshot_manifest(const Json& value, SnapshotManifest& out, std::string& phase, uint32_t& next) {
    if (!closed(value, {"transfer_id", "phase", "owner_octopus_id", "owner_boot_id", "config_revision", "total_length", "payload_hash", "document_kind", "chunk_count", "next_index"})) return Error::invalid_message;
    SnapshotManifest m; uint64_t length, count, index;
    if (!id(value.get("transfer_id"), m.transfer_id) || !id(value.get("owner_octopus_id"), m.owner_octopus_id) || !id(value.get("owner_boot_id"), m.owner_boot_id) ||
        !decimal(value.get("config_revision"), m.config_revision) || !json_uint(value.get("total_length"), length, 1, 32768) ||
        !json_string(value.get("payload_hash"), m.payload_hash, 64, 64) || !json_string(value.get("document_kind"), m.document_kind, 1, 32) ||
        !json_string(value.get("phase"), phase, 1, 16) || !json_uint(value.get("chunk_count"), count, 1, 16) || !json_uint(value.get("next_index"), index, 0, 16)) return Error::invalid_message;
    m.total_length = static_cast<uint32_t>(length);
    if (!valid(m) || count != m.chunk_count() || index > count ||
        (phase != "begin" && phase != "ack" && phase != "complete" && phase != "reject") ||
        (phase == "begin" && index != 0) || (phase == "complete" && index != count)) return Error::invalid_message;
    out = std::move(m); next = static_cast<uint32_t>(index); return Error::none;
}
std::string snapshot_manifest_json(const SnapshotManifest& m, std::string_view phase, uint32_t next) {
    if (!valid(m) || next > m.chunk_count() || (phase != "begin" && phase != "ack" && phase != "complete" && phase != "reject") ||
        (phase == "begin" && next) || (phase == "complete" && next != m.chunk_count())) return {};
    return "{\"transfer_id\":\"" + m.transfer_id.str() + "\",\"phase\":\"" + std::string(phase) + "\",\"owner_octopus_id\":\"" + m.owner_octopus_id.str() +
        "\",\"owner_boot_id\":\"" + m.owner_boot_id.str() + "\",\"config_revision\":\"" + model::format_uint64(m.config_revision) +
        "\",\"total_length\":" + std::to_string(m.total_length) + ",\"payload_hash\":\"" + m.payload_hash + "\",\"document_kind\":\"" + m.document_kind +
        "\",\"chunk_count\":" + std::to_string(m.chunk_count()) + ",\"next_index\":" + std::to_string(next) + "}";
}
std::string snapshot_chunk_json(const SnapshotManifest& m, uint32_t index, std::string_view payload) {
    if (!valid(m) || payload.size() != m.total_length || index >= m.chunk_count()) return {};
    const auto offset = static_cast<size_t>(index) * 2048; const auto data = payload.substr(offset, std::min<size_t>(2048, payload.size() - offset));
    static constexpr char hex[] = "0123456789abcdef"; std::string encoded; encoded.reserve(data.size() * 2);
    for (unsigned char c : data) { encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    return "{\"transfer_id\":\"" + m.transfer_id.str() + "\",\"index\":" + std::to_string(index) + ",\"total_length\":" + std::to_string(m.total_length) +
        ",\"payload_hash\":\"" + m.payload_hash + "\",\"data\":\"" + encoded + "\"}";
}
void SnapshotTransfer::reset() { manifest_ = {}; payload_.clear(); started_ms_ = last_progress_ms_ = 0; next_ = 0; begun_ = complete_ = false; }
Error SnapshotTransfer::begin(const SnapshotManifest& m, model::OctopusId owner, model::BootId boot, uint64_t revision, uint64_t now) {
    if (begun_) return Error::transaction_conflict;
    if (!hash_ || !valid(m)) return Error::invalid_message;
    if (m.owner_octopus_id != owner) return Error::target_not_local;
    if (m.owner_boot_id != boot) return Error::stale_boot;
    if (m.config_revision != revision) return Error::stale_config;
    manifest_ = m; payload_.reserve(m.total_length); started_ms_ = last_progress_ms_ = now; begun_ = true; return Error::none;
}
Error SnapshotTransfer::accept(const Json& chunk, uint64_t now) {
    if (!begun_) return Error::not_ready;
    if (now < last_progress_ms_ || now - started_ms_ >= 30000 || now - last_progress_ms_ >= 5000) { reset(); return Error::not_ready; }
    model::MessageId transfer; uint64_t index, total; std::string hash, data;
    if (!closed(chunk, {"transfer_id", "index", "total_length", "payload_hash", "data"}) || !id(chunk.get("transfer_id"), transfer) ||
        !json_uint(chunk.get("index"), index, 0, 15) || !json_uint(chunk.get("total_length"), total, 1, 32768) ||
        !json_string(chunk.get("payload_hash"), hash, 64, 64) || !json_string(chunk.get("data"), data, 2, 4096) || data.size() % 2 ||
        transfer != manifest_.transfer_id || total != manifest_.total_length || hash != manifest_.payload_hash) return Error::invalid_message;
    const auto offset = static_cast<size_t>(index) * 2048;
    if (index >= manifest_.chunk_count() || data.size() / 2 != std::min<size_t>(2048, total - offset) || index > next_) return Error::invalid_message;
    std::string decoded; decoded.reserve(data.size() / 2);
    for (size_t n = 0; n < data.size(); n += 2) { const auto high = nibble(data[n]), low = nibble(data[n + 1]); if (high < 0 || low < 0) return Error::invalid_message; decoded += static_cast<char>((high << 4) | low); }
    if (index < next_) return payload_.compare(offset, decoded.size(), decoded) == 0 ? Error::duplicate : Error::invalid_message;
    payload_ += decoded; ++next_; last_progress_ms_ = now;
    if (next_ == manifest_.chunk_count()) {
        if (payload_.size() != manifest_.total_length || hash_(payload_) != manifest_.payload_hash) { reset(); return Error::invalid_message; }
        complete_ = true;
    }
    return Error::none;
}
} // namespace amity::consortium
