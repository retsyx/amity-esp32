// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/update.hpp"
#include <algorithm>
namespace amity::consortium {
namespace {
bool text(const Json* v, std::initializer_list<std::string_view> values) {
    return v && v->type == Json::Type::string && std::find(values.begin(), values.end(), v->string) != values.end();
}
bool hex(const Json* v, size_t length) {
    return v && v->type == Json::Type::string && v->string.size() == length &&
        std::all_of(v->string.begin(), v->string.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool release(const Json* v) {
    return v && v->type == Json::Type::string && !v->string.empty() && v->string.size() <= 96 &&
        std::all_of(v->string.begin(), v->string.end(), [](char c) { return (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'; });
}
bool base(const Json& v) {
    model::MessageId request;
    return id(v.get("request_id"), request) && release(v.get("release_id")) && hex(v.get("image_sha256"), 64);
}
bool offset(const Json& v) { uint64_t n = 0; return decimal(v.get("offset"), n); }
}
bool is_update_kind(Kind k) {
    return k == Kind::update_begin || k == Kind::update_chunk || k == Kind::update_install ||
        k == Kind::update_status || k == Kind::update_abort || k == Kind::update_result;
}
bool valid_update(Kind k, const Json& v) {
    if (!base(v)) return false;
    if (k == Kind::update_begin) {
        std::string manifest;
        return closed(v, {"request_id", "release_id", "image_sha256", "canonical_manifest", "signature_hex"}) &&
            json_string(v.get("canonical_manifest"), manifest, 1, 4096) && (hex(v.get("signature_hex"), 128) || hex(v.get("signature_hex"), 0));
    }
    if (k == Kind::update_chunk) {
        const auto* data = v.get("data_hex");
        return closed(v, {"request_id", "release_id", "image_sha256", "offset", "data_hex", "chunk_sha256"}) && offset(v) &&
            data && data->type == Json::Type::string && !data->string.empty() && data->string.size() <= 4096 &&
            data->string.size() % 2 == 0 && hex(data, data->string.size()) && hex(v.get("chunk_sha256"), 64);
    }
    if (k == Kind::update_install) {
        if (text(v.get("phase"), {"stage"})) return closed(v, {"request_id", "release_id", "image_sha256", "phase"});
        uint64_t delay = 0;
        return text(v.get("phase"), {"activate"}) && closed(v, {"request_id", "release_id", "image_sha256", "phase", "reboot_delay_ms"}) &&
            decimal(v.get("reboot_delay_ms"), delay) && delay <= 15000;
    }
    if (k == Kind::update_status)
        return closed(v, {"request_id", "release_id", "image_sha256"});
    if (k == Kind::update_abort)
        return closed(v, {"request_id", "release_id", "image_sha256"}, {"reason"}) &&
            (!v.get("reason") || text(v.get("reason"), {"cancelled", "timeout", "link_lost", "failed"}));
    if (k != Kind::update_result || !closed(v, {"request_id", "release_id", "image_sha256", "offset", "stage", "error"},
        {"boot_id", "image_elf_sha256", "version", "local_healthy", "config_revision", "policy_hash", "update_phase"}) || !offset(v) ||
        !text(v.get("stage"), {"ready", "progress", "staged", "installing", "aborted", "rejected", "status"}) ||
        !text(v.get("error"), {"", "INVALID_MESSAGE", "NOT_READY", "UNAUTHORIZED_ROLE", "CONFLICT", "INVALID_MANIFEST", "INVALID_SIGNATURE",
            "WRONG_TARGET", "HASH_MISMATCH", "OFFSET_MISMATCH", "STORAGE_FAILURE", "TIMEOUT", "INTERNAL_ERROR"})) return false;
    model::BootId boot; std::string version; bool healthy = false; uint64_t revision = 0;
    return (!v.get("boot_id") || id(v.get("boot_id"), boot)) &&
        (!v.get("image_elf_sha256") || hex(v.get("image_elf_sha256"), 64)) &&
        (!v.get("version") || json_string(v.get("version"), version, 1, 64)) &&
        (!v.get("local_healthy") || json_bool(v.get("local_healthy"), healthy)) &&
        (!v.get("config_revision") || decimal(v.get("config_revision"), revision)) &&
        (!v.get("policy_hash") || hex(v.get("policy_hash"), 64)) &&
        (!v.get("update_phase") || text(v.get("update_phase"), {"idle", "staged", "staged_image", "installing", "boot_checking", "healthy", "failed", "aborted", "interrupted", "rolled_back", "recovery_required"}));
}
}
