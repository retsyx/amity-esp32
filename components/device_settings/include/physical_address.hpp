#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace device_settings {
constexpr uint16_t default_front_physical_address = 0x1000;
// CEC topology is four hexadecimal nibbles; zeros may only trail nonzero ports.
inline bool parse_front_physical_address(std::string_view text, uint16_t &address) {
    if(text.empty()) { address=default_front_physical_address; return true; }
    if(text.size()!=7)return false;
    uint16_t value=0;
    bool trailing_zero=false;
    for(unsigned i=0;i<4;++i) {
        if(i&&text[2*i-1]!='.')return false;
        char c=text[2*i];
        unsigned nibble;
        if(c>='0'&&c<='9')nibble=c-'0';
        else if(c>='a'&&c<='f')nibble=c-'a'+10;
        else if(c>='A'&&c<='F')nibble=c-'A'+10;
        else return false;
        if(trailing_zero&&nibble)return false;
        if(!nibble)trailing_zero=true;
        value=static_cast<uint16_t>((value<<4)|nibble);
    }
    if(value==0xffff)return false; // Reserved invalid/unknown address.
    address=value;
    return true;
}
inline std::string format_physical_address(uint16_t address) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string text="0.0.0.0";
    for(unsigned i=0;i<4;++i)text[2*i]=hex[(address>>(12-4*i))&15];
    return text;
}
}
