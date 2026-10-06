#include "configuration.hpp"
#include <cassert>
#include <iostream>
int main() {
    web_admin::Configuration c;std::string e;
    auto parse=[&](const std::string &s){return web_admin::parse_configuration(s,c,e);};
    assert(parse(R"({"version":1,"activities":[{"name":"Game","display":"TV","source":"PS5","audio":"AVR","switch":{"device":"AVR","input":4}}],"quirks":[{"vendor_id":2048,"power_on":[68,109]}]})"));
    assert(c.activities[0].switch_input==4&&c.quirks[0].power_on[1]==109);
    assert(!parse(R"({"version":2,"activities":[]})"));
    assert(!parse(R"({"version":1,"version":1,"activities":[]})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A"},{"name":"A"}]})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A","switch":{"device":"AVR","input":1.5}}]})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A","source":5}]})"));
    assert(!parse(R"({"version":1,"activities":[]} trailing)"));
    assert(!parse(std::string(100,'[')+std::string(100,']')));
    assert(!parse(std::string(32769,' ')));
    assert(!parse(std::string("{\"version\":1,\"activities\":[]}")+std::string(1,'\0')));
    assert(parse(R"({"version":1,"activities":[]})"));
    assert(!parse(R"({"version":1,"activities":[],"hostname":"other"})"));
    assert(!parse(R"({"version":1,"activities":[],"matter":{"fabrics":[]}})"));
    assert(!parse(R"({"version":1,"activities":[],"admin_auth":"secret"})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A","hostname":"other"}]})"));
    assert(!parse(R"({"version":1,"activities":[],"quirks":[{"vendor_id":1,"matter":{}}]})"));
    std::cout<<"Configuration validation tests passed\n";
}
