// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
namespace amity::consortium {
// Closed peer-update messages. Firmware crypto/storage remain actuator-local;
// no network message can replace its compiled verifier or skip manifest checks.
// The main updates members running other firmware through these messages, so
// they are frozen. Every request carries request_id and, besides it:
//   update_begin    canonical_manifest, signature_hex
//   update_chunk    the binary chunk below
//   update_install  release_id and phase "stage", or "activate" with reboot_delay_ms
//   update_abort    release_id
// The member answers each JSON request with update_result {request_id, stage},
// stage being ready, staged, installing, aborted or rejected.
bool is_update_kind(Kind);
bool valid_update(Kind, const Json&);
constexpr size_t update_chunk_bytes = 2048;
// Binary chunks bind to the accepted begin request. Integers are big-endian.
// Chunk payload: begin ID (16), offset (8), then 1..2048 image bytes.
// Frame: AFU1 (4), configuration revision (8), TTL (4), chunk payload.
// The existing authenticated TLS frame length delimits the image bytes.
bool valid_update_chunk(const UpdateChunk&);
std::string encode_update_chunk(model::MessageId begin,uint64_t offset,std::string_view bytes);
bool decode_update_chunk(std::string_view encoded,UpdateChunk&);
std::string update_chunk_envelope(const Envelope&,std::string_view encoded_chunk);
Error parse_update_chunk_envelope(std::string_view,Envelope&);
}
