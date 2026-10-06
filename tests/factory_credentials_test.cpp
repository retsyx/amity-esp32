#include "factory_credentials.hpp"
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    assert(argc == 4);
    std::ifstream file(argv[1]);
    std::string hex;
    file >> hex;
    assert(hex.size() == 192);
    std::array<uint8_t, 96> golden{};
    for (size_t i = 0; i < golden.size(); ++i)
        golden[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
    amity::FactoryCredentials credentials{};
    auto parse = [&](const auto &data, uint16_t vendor = 0xfff1, uint16_t product = 0x8000) {
        return amity::parse_factory_record(data.data(), data.size(), golden.data() + 64,
                                          vendor, product, credentials);
    };
    assert(parse(golden) == amity::FactoryRecord::Valid);
    assert(credentials.passcode == 20202021 && credentials.discriminator == 3840);
    assert(std::strcmp(credentials.hostname, "amity") == 0);
    for (size_t i = 0; i < 32; ++i) assert(credentials.salt[i] == i);
    assert(parse(golden, 1) == amity::FactoryRecord::Invalid);
    assert(parse(golden, 0xfff1, 1) == amity::FactoryRecord::Invalid);
    assert(amity::parse_factory_record(golden.data(), 95, golden.data()+64, 0xfff1, 0x8000,
                                      credentials) == amity::FactoryRecord::Invalid);
    for (size_t offset : {0, 8, 10, 12, 14, 22, 24, 60, 64, 95}) {
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
    for (uint32_t pin : {0u, 12345678u, 87654321u, 99999999u, 0xffffffffu})
        assert(!amity::valid_setup_pin(pin));
    for (uint32_t i = 1; i <= 9; ++i) assert(!amity::valid_setup_pin(i * 11111111));
    std::ifstream file2(argv[2]);file2 >> hex;
    assert(hex.size() == 320);
    std::array<uint8_t,160> v2{};
    for (size_t i=0;i<v2.size();++i) v2[i]=std::stoul(hex.substr(i*2,2),nullptr,16);
    auto parse2 = [&](const auto &record) {
        return amity::parse_factory_record(record.data(),record.size(),v2.data()+128,0xfff1,0x8000,credentials);
    };
    assert(parse2(v2) == amity::FactoryRecord::Valid);
    assert(std::strcmp(credentials.hostname,"living-room") == 0);
    assert(amity::factory_digest_offset(v2.data(),159) == 0);
    auto wrong=v2;wrong[128]^=1;
    assert(parse2(wrong) == amity::FactoryRecord::Invalid);
    for (const auto *name : {"", "-room", "room-", "room.local", "a_b", "a b", "ROOM", "abcdefghijklmnopqrstuvwxyz1234567"}) {
        wrong=v2;std::memset(wrong.data()+64,0,64);std::memcpy(wrong.data()+64,name,std::strlen(name));
        assert(parse2(wrong) == amity::FactoryRecord::Invalid);
    }
    wrong=v2;std::memset(wrong.data()+64,'a',64);
    assert(parse2(wrong) == amity::FactoryRecord::Invalid);
    wrong=v2;wrong[127]=1;
    assert(parse2(wrong) == amity::FactoryRecord::Invalid);
    wrong=v2;std::memset(wrong.data()+64,0,64);std::memset(wrong.data()+64,'a',32);
    assert(parse2(wrong) == amity::FactoryRecord::Valid);
    // A v1 record read into the larger v2 buffer still defaults to amity.
    wrong.fill(0xff);std::memcpy(wrong.data(),golden.data(),golden.size());
    assert(parse(wrong) == amity::FactoryRecord::Valid);
    assert(std::strcmp(credentials.hostname,"amity") == 0);
    std::ifstream file3(argv[3]);file3 >> hex;
    assert(hex.size() == 576);
    std::array<uint8_t,288> v3{};
    for(size_t i=0;i<v3.size();++i)v3[i]=std::stoul(hex.substr(i*2,2),nullptr,16);
    auto parse3=[&](const auto &record){return amity::parse_factory_record(record.data(),record.size(),v3.data()+256,0xfff1,0x8000,credentials);};
    assert(parse3(v3)==amity::FactoryRecord::Valid);
    assert(std::strlen(credentials.admin_auth)==97&&credentials.admin_auth[32]==':');
    auto bad3=v3;bad3[226]=1;assert(parse3(bad3)==amity::FactoryRecord::Invalid);
    for(size_t i:{128,160,224,225,256,287}){bad3=v3;bad3[i]=0xff;assert(parse3(bad3)==amity::FactoryRecord::Invalid);}
    bad3=v3;std::memset(bad3.data()+128,0,98);assert(parse3(bad3)==amity::FactoryRecord::Valid);
    assert(credentials.admin_auth[0]==0);
    assert(amity::factory_digest_offset(v3.data(),287)==0);
    std::cout << "PASS factory record parser and Go/Python/C++ golden format\n";
}
