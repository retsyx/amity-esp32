// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "hex.hpp"
#include "amity/system/model.hpp"
namespace amity::consortium::platform {
std::string hex(std::string_view bytes){return amity::system::hex(bytes);}
bool unhex(std::string_view value,std::string &out,size_t cap){if(value.size()%2||value.size()/2>cap)return false;out.clear();out.reserve(value.size()/2);auto nibble=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};for(size_t i=0;i<value.size();i+=2){int a=nibble(value[i]),b=nibble(value[i+1]);if(a<0||b<0){out.clear();return false;}out.push_back(char((a<<4)|b));}return true;}
} // namespace
