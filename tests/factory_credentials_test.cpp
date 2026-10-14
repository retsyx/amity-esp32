/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "factory_credentials.hpp"
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream file(argv[1]);
    std::string hex;
    file >> hex;
    assert(hex.size() == amity::factory_record_size * 2);
    std::array<uint8_t, amity::factory_record_size> golden{};
    for (size_t i = 0; i < golden.size(); ++i)
        golden[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
    amity::FactoryCredentials credentials{};
    auto parse = [&](const auto &data, uint16_t vendor = 0xfff1, uint16_t product = 0x8000) {
        return amity::parse_factory_record(data.data(), data.size(), golden.data() + amity::factory_record_digest_offset,
                                          vendor, product, credentials);
    };
    assert(parse(golden) == amity::FactoryRecord::Valid);
    assert(credentials.passcode == 20202021 && credentials.discriminator == 3840);
    assert(std::strcmp(credentials.hostname, "living-room") == 0);
    assert(std::strlen(credentials.admin_auth) == 97 && credentials.admin_auth[32] == ':');
    for (size_t i = 0; i < 32; ++i) assert(credentials.salt[i] == i);
    assert(parse(golden, 1) == amity::FactoryRecord::Invalid);
    assert(parse(golden, 0xfff1, 1) == amity::FactoryRecord::Invalid);
    assert(amity::parse_factory_record(golden.data(), golden.size() - 1,
                                      golden.data() + amity::factory_record_digest_offset, 0xfff1, 0x8000,
                                      credentials) == amity::FactoryRecord::Invalid);
    for (size_t offset : {0, 8, 10, 12, 14, 22, 24, 60, 127, 226, 256, 287}) {
        auto bad = golden;
        bad[offset] ^= 1;
        assert(parse(bad) == amity::FactoryRecord::Invalid);
    }
    auto bad = golden;
    bad[21] = 0xff; // discriminator above 12 bits
    assert(parse(bad) == amity::FactoryRecord::Invalid);
    bad = golden;
    std::memset(bad.data()+16, 0, 4);
    assert(parse(bad) == amity::FactoryRecord::Invalid);
    bad.fill(0xff);
    assert(parse(bad) == amity::FactoryRecord::Missing);
    bad[0] = 0;
    assert(parse(bad) == amity::FactoryRecord::Invalid);
    // Invalid input never modifies the last accepted credentials.
    assert(credentials.passcode == 20202021 && credentials.discriminator == 3840);
    assert(std::strcmp(credentials.hostname, "living-room") == 0);
    assert(std::strlen(credentials.admin_auth) == 97 && credentials.admin_auth[32] == ':');
    for (uint32_t pin : {0u, 12345678u, 87654321u, 99999999u, 0xffffffffu})
        assert(!amity::valid_setup_pin(pin));
    for (uint32_t i = 1; i <= 9; ++i) assert(!amity::valid_setup_pin(i * 11111111));
    for (uint8_t version : {0, 1, 2, 4}) {
        bad = golden;
        bad[8] = version;
        assert(parse(bad) == amity::FactoryRecord::Invalid);
    }
    // Earlier record layouts remain invalid when read into the current record buffer.
    for (const auto &header : {std::array<uint8_t, 3>{1, 96, 0}, std::array<uint8_t, 3>{2, 160, 0}}) {
        bad = golden;
        bad[8] = header[0];
        bad[10] = header[1];
        bad[11] = header[2];
        assert(parse(bad) == amity::FactoryRecord::Invalid);
    }
    for (const auto *name : {"", "-room", "room-", "room.local", "a_b", "a b", "ROOM", "abcdefghijklmnopqrstuvwxyz1234567"}) {
        bad = golden;
        std::memset(bad.data() + 64, 0, 64);
        std::memcpy(bad.data() + 64, name, std::strlen(name));
        assert(parse(bad) == amity::FactoryRecord::Invalid);
    }
    bad = golden;
    std::memset(bad.data() + 64, 'a', 64);
    assert(parse(bad) == amity::FactoryRecord::Invalid);
    bad = golden;
    std::memset(bad.data() + 64, 0, 64);
    std::memset(bad.data() + 64, 'a', 32);
    assert(parse(bad) == amity::FactoryRecord::Valid);
    for (size_t i : {128, 160, 224, 225}) {
        bad = golden;
        bad[i] = 0xff;
        assert(parse(bad) == amity::FactoryRecord::Invalid);
    }
    bad = golden;
    std::memset(bad.data() + 128, 0, 98);
    assert(parse(bad) == amity::FactoryRecord::Valid);
    assert(credentials.admin_auth[0] == 0);
    std::cout << "PASS factory record parser and Go/Python/C++ golden format\n";
}
