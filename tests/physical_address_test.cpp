#include "physical_address.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace device_settings;
    uint16_t address = 0;
    assert(parse_front_physical_address("", address) && address == 0x1000);
    // Round-trip every valid topology; reject gaps and the reserved unknown address.
    for (unsigned value = 0; value <= 0xffff; ++value) {
        bool valid = value != 0xffff, zero = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned nibble = (value >> shift) & 15;
            if (zero && nibble) valid = false;
            if (!nibble) zero = true;
        }
        address = 0x1234;
        assert(parse_front_physical_address(format_physical_address(value), address) == valid);
        assert(address == (valid ? value : 0x1234));
    }
    assert(parse_front_physical_address("a.b.c.0", address) && address == 0xabc0);
    for (auto text : {"1000", "1.0.0", "1:0:0:0", "1.0.0.0 ", " 1.0.0.0", "g.0.0.0", "10.0.0.0", " "})
        assert(!parse_front_physical_address(text, address));
    assert(!parse_front_physical_address(std::string("1.0.0.0\0tail", 12), address));
    std::cout << "PASS physical address validation, defaults and exhaustive round trips\n";
}
