/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "configuration.hpp"
#include "json_input.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
    auto accepts_json=[](const std::string &json){auto *root=web_admin::parse_json_input(json);const bool accepted=root!=nullptr;cJSON_Delete(root);return accepted;};
    for(const auto *json:{R"({"password":"x\u0000long-secret-suffix"})",
                         R"({"pass\u0000word":"secret"})",
                         R"({"nested":[{"value":"\u0000"}]})",
                         R"({"password":"prefix\\\u0000suffix"})"})assert(!accepts_json(json));
    assert(!accepts_json(std::string("{\"password\":\"x")+std::string(1,'\0')+"suffix\"}"));
    assert(!accepts_json(std::string(9,'[')+std::string(9,']')));
    assert(!accepts_json(R"({"password":"secret"} trailing)"));
    for(const auto *json:{R"({"password":"literal\\u0000"})",
                         R"({"password":"literal\\\\u0000"})",
                         R"({"password":"quote\" and \\u0000"})",
                         R"({"password":"\u005cu0000"})",
                         R"({"password":"\u0001\u0041\ud83d\ude00"})"})assert(accepts_json(json));
    auto *password_json=web_admin::parse_json_input(R"({"password":"literal\\u0000"})");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(password_json,"password")->valuestring,R"(literal\u0000)"));
    cJSON_Delete(password_json);
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
    assert(!parse(R"({"version":1,"activities":[{"name":"A\u0000B"}]})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A","source":"TV\u0000Other"}]})"));
    assert(!parse(R"({"version":1,"activities":[{"name\u0000ignored":"A"}]})"));
    assert(parse(R"({"version":1,"activities":[{"name":"A","source":"TV\\u0000Other"}]})"));
    assert(c.activities[0].source==R"(TV\u0000Other)");
    assert(parse(R"({"version":1,"activities":[]})"));
    assert(!parse(R"({"version":1,"activities":[],"hostname":"other"})"));
    assert(!parse(R"({"version":1,"activities":[],"matter":{"fabrics":[]}})"));
    assert(!parse(R"({"version":1,"activities":[],"admin_auth":"secret"})"));
    assert(!parse(R"({"version":1,"activities":[{"name":"A","hostname":"other"}]})"));
    assert(!parse(R"({"version":1,"activities":[],"quirks":[{"vendor_id":1,"matter":{}}]})"));
    std::cout<<"Configuration validation tests passed\n";
}
