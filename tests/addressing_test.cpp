#include "amity/system/model.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <limits>
#include <type_traits>
using namespace amity::system;
template<class T>T id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return T::parse(text).value();}
CecPortRef port(unsigned slot=0,Side side=Side::back,unsigned board=2){return {id<ConsortiumId>(1),id<OctopusId>(board),static_cast<uint8_t>(slot),side};}
void codecs() {
    static_assert(!std::is_convertible<QualifiedDeviceName, FullyQualifiedCecAddress>::value);
    static_assert(!std::is_assignable<OctopusId&, ConsortiumId>::value);
    assert(!OctopusId::parse("00000000000000000000000000000000"));
    assert(!OctopusId::parse("A0000000000000000000000000000001"));
    assert(!OctopusId::parse("0000000000000000000000000000001"));
    assert(parse_uint64("18446744073709551615") == std::numeric_limits<uint64_t>::max());
    for (auto bad : {"18446744073709551616", "00", "01", "-1", "+1", " 1", "1.0", "1e1", ""}) assert(!parse_uint64(bad));
    QualifiedDeviceName name{port(), "A/B TV"};
    assert(parse_name_json(name_json(name)) == name);
    for (uint8_t t = 0; t < 4; ++t) for (auto side : {Side::front, Side::back}) for (uint8_t la = 0; la < 15; ++la) {
        FullyQualifiedCecAddress a{port(t, side), la};
        assert(!address_json(a).empty() && !parse_name_json(address_json(a)));
    }
    assert(port(3).valid()&&!port(4).valid());
    auto invalid_json=name_json({port(3),"Player"});const auto index=invalid_json.find("\"tentacle_id\":3");
    assert(index!=std::string::npos);invalid_json[index+14]='4';assert(!parse_name_json(invalid_json));
    auto json = name_json(name);
    assert(!parse_name_json(json.substr(0, json.size() - 1) + ",\"logical_address\":4}"));
    assert(!parse_name_json(json.substr(0, json.size() - 1) + ",\"side\":\"front\"}"));
    assert(!parse_name_json("{}"));
    assert((QualifiedDeviceName{port(), " Apple TV "}.valid()));
    assert((!QualifiedDeviceName{port(), "bad\nname"}.valid()));
}
int main(){codecs();std::cout<<"PASS exact equipment address codecs: stable typed IDs, adapter/side scope, OSD selectors, runtime CEC addresses and strict canonical values\n";}
