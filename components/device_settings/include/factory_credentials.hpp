/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amity {
constexpr size_t factory_record_size = 288;
constexpr size_t factory_record_digest_offset = factory_record_size - 32;
enum class FactoryRecord { Missing, Valid, Invalid };
struct FactoryCredentials {
    uint32_t passcode;
    uint16_t discriminator;
    uint8_t salt[32];
    char hostname[64];
    char admin_auth[98];
};
inline bool valid_setup_pin(uint32_t pin) {
    if (!pin || pin > 99999998 || pin == 12345678 || pin == 87654321) return false;
    for (uint32_t d = 1; d <= 9; ++d) if (pin == d * 11111111) return false;
    return true;
}
inline bool valid_factory_hostname(const uint8_t *name) {
    size_t length = 0;
    while (length < 64 && name[length]) {
        const auto c = name[length++];
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return false;
    }
    // esp_netif_set_hostname supports at most 32 bytes on this SDK.
    if (!length || length > 32 || name[0] == '-' || name[length - 1] == '-') return false;
    for (size_t i = length; i < 64; ++i) if (name[i]) return false;
    return true;
}
inline bool valid_admin_auth(const uint8_t *record) {
    bool empty = true;
    for (size_t i = 0; i < 98; ++i) empty &= record[i] == 0;
    if (empty) return true;
    if (record[32] != ':' || record[97]) return false;
    for (size_t i = 0; i < 97; ++i) {
        if (i == 32) continue;
        const auto c = record[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}
// digest is SHA-256 of the bytes preceding the record's checksum.
inline FactoryRecord parse_factory_record(const uint8_t *data, size_t size, const uint8_t digest[32],
                                          uint16_t vendor, uint16_t product, FactoryCredentials &out) {
    if (size < factory_record_size) return FactoryRecord::Invalid;
    bool erased = true;
    for (size_t i = 0; i < size; ++i) erased &= data[i] == 0xff;
    if (erased) return FactoryRecord::Missing;
    auto u16 = [&](size_t i) { return uint16_t(data[i] | uint16_t(data[i + 1]) << 8); };
    auto u32 = [&](size_t i) { return uint32_t(u16(i)) | uint32_t(u16(i + 2)) << 16; };
    if (memcmp(data, "AMITYPRV", 8) ||
        u16(8) != 3 || u16(10) != factory_record_size ||
        u16(12) != vendor || u16(14) != product || !valid_setup_pin(u32(16)) ||
        u16(20) > 4095 || u16(22) || u32(24) != 1000 || u32(60) ||
        memcmp(data + factory_record_digest_offset, digest, 32) ||
        !valid_factory_hostname(data + 64) || !valid_admin_auth(data + 128)) return FactoryRecord::Invalid;
    for (size_t i = 226; i < factory_record_digest_offset; ++i) if (data[i]) return FactoryRecord::Invalid;
    out.passcode = u32(16);
    out.discriminator = u16(20);
    memcpy(out.salt, data + 28, sizeof(out.salt));
    memcpy(out.hostname, data + 64, sizeof(out.hostname));
    memcpy(out.admin_auth, data + 128, sizeof(out.admin_auth));
    return FactoryRecord::Valid;
}
} // namespace amity
