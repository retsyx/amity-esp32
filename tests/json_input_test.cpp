/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

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
    std::cout<<"JSON input validation tests passed\n";
}
