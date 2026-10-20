// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/json.hpp"
#include <algorithm>
#include <limits>

namespace amity::consortium {
const Json *Json::get(std::string_view key) const {
    if (type != Type::object) return nullptr;
    for (const auto &entry : object) if (entry.first == key) return &entry.second;
    return nullptr;
}
Json *Json::get(std::string_view key) {
    if (type != Type::object) return nullptr;
    for (auto &entry : object) if (entry.first == key) return &entry.second;
    return nullptr;
}
namespace {
bool utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i++]);
        if (!lead) return false;
        if (lead < 0x80) continue;
        unsigned count = 0; uint32_t cp = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { count=1; cp=lead&31; minimum=0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count=2; cp=lead&15; minimum=0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count=3; cp=lead&7; minimum=0x10000; }
        else return false;
        if (s.size()-i < count) return false;
        while (count--) { const auto c=static_cast<unsigned char>(s[i++]); if ((c&0xc0)!=0x80) return false; cp=(cp<<6)|(c&63); }
        if (cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
class Parser {
public:
    Parser(std::string_view s, JsonError &e,size_t maximum,size_t values) : s_(s), error_(e),maximum_(maximum),maximum_values_(values) {}
    bool parse(Json &out) {
        if (s_.empty() || s_.size()>maximum_ || !utf8(s_)) return fail("invalid_size_or_utf8");
        if (!value(out, 0)) return false;
        ws(); return pos_==s_.size() || fail("trailing_content");
    }
private:
    bool fail(const char *s) { error_={pos_,s}; return false; }
    void ws() { while (pos_<s_.size() && (s_[pos_]==' ' || s_[pos_]=='\n' || s_[pos_]=='\r' || s_[pos_]=='\t')) ++pos_; }
    bool take(char c) { if (pos_<s_.size() && s_[pos_]==c) {++pos_;return true;} return false; }
    bool hex4(uint32_t &n) {
        n=0; for (unsigned i=0;i<4;++i) { if (pos_==s_.size()) return false; char c=s_[pos_++]; unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16; if (d==16) return false; n=(n<<4)|d; } return true;
    }
    void append(std::string &out,uint32_t cp) {
        if (cp<0x80) out.push_back(static_cast<char>(cp));
        else if(cp<0x800) {out.push_back(static_cast<char>(0xc0|(cp>>6)));out.push_back(static_cast<char>(0x80|(cp&63)));}
        else if(cp<0x10000) {out.push_back(static_cast<char>(0xe0|(cp>>12)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}
        else {out.push_back(static_cast<char>(0xf0|(cp>>18)));out.push_back(static_cast<char>(0x80|((cp>>12)&63)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}
    }
    bool string(std::string &out) {
        if (!take('"')) return fail("expected_string");
        out.clear();
        while(pos_<s_.size()) {
            const unsigned char c=static_cast<unsigned char>(s_[pos_++]);
            if(c=='"') return true;
            if(c<32) return fail("control_in_string");
            if(c!='\\') out.push_back(static_cast<char>(c));
            else {
                if(pos_==s_.size()) return fail("incomplete_escape");
                char e=s_[pos_++];
                switch(e) {
                case '"':case '\\':case '/':out.push_back(e);break;
                case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;
                case 'u': {uint32_t cp=0; if(!hex4(cp)||!cp) return fail("invalid_unicode");
                    if(cp>=0xd800&&cp<=0xdbff) {uint32_t low=0; if(!take('\\')||!take('u')||!hex4(low)||low<0xdc00||low>0xdfff) return fail("invalid_surrogate"); cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}
                    else if(cp>=0xdc00&&cp<=0xdfff) return fail("invalid_surrogate");
                    append(out,cp);break;}
                default:return fail("invalid_escape");
                }
            }
            if(out.size()>4096) return fail("string_limit");
        }
        return fail("incomplete_string");
    }
    bool literal(std::string_view word) { if(s_.substr(pos_,word.size())!=word) return fail("invalid_literal"); pos_+=word.size(); return true; }
    bool value(Json &out,unsigned depth) {
        ws(); if(depth>12 || ++nodes_>maximum_values_) return fail("complexity_limit");
        if(pos_==s_.size()) return fail("missing_value");
        const char c=s_[pos_];
        if(c=='{') {
            ++pos_;out.type=Json::Type::object;ws();if(take('}')) return true;
            do { ws(); std::string key; if(!string(key))return false;
                for(const auto &v:out.object) if(v.first==key)return fail("duplicate_key");
                ws();if(!take(':'))return fail("missing_colon"); Json child;if(!value(child,depth+1))return false;out.object.emplace_back(std::move(key),std::move(child));ws();
                if(take('}'))return true;
            } while(take(','));return fail("invalid_object");
        }
        if(c=='[') {++pos_;out.type=Json::Type::array;ws();if(take(']'))return true;do {Json child;if(!value(child,depth+1))return false;out.array.push_back(std::move(child));ws();if(take(']'))return true;}while(take(','));return fail("invalid_array");}
        if(c=='"') {out.type=Json::Type::string;return string(out.string);}
        if(c=='t'||c=='f') {out.type=Json::Type::boolean;out.boolean=c=='t';return literal(c=='t'?"true":"false");}
        if(c=='n') {out.type=Json::Type::null_value;return literal("null");}
        if(c<'0'||c>'9')return fail("expected_unsigned_integer");
        out.type=Json::Type::integer;const size_t begin=pos_;
        while(pos_<s_.size()&&s_[pos_]>='0'&&s_[pos_]<='9') {unsigned d=s_[pos_++]-'0';if(out.integer>(UINT64_MAX-d)/10)return fail("integer_overflow");out.integer=out.integer*10+d;}
        if(pos_-begin>1&&c=='0')return fail("leading_zero");
        return true;
    }
    std::string_view s_;JsonError &error_;size_t pos_=0,nodes_=0,maximum_=max_frame_bytes,maximum_values_=256;
};
}
bool parse_json(std::string_view bytes, Json &out, JsonError &error,size_t maximum,size_t values) { Json parsed; if(maximum>32768||values>4096||!values||!Parser(bytes,error,maximum,values).parse(parsed))return false; out=std::move(parsed);error={};return true; }
bool closed(const Json &v,std::initializer_list<std::string_view> required,std::initializer_list<std::string_view> optional) {
    if(v.type!=Json::Type::object)return false;
    for(auto key:required)if(!v.get(key))return false;
    for(const auto &entry:v.object)if(std::find(required.begin(),required.end(),entry.first)==required.end()&&std::find(optional.begin(),optional.end(),entry.first)==optional.end())return false;
    return true;
}
bool json_string(const Json *v,std::string &out,size_t min,size_t max) {if(!v||v->type!=Json::Type::string||v->string.size()<min||v->string.size()>max)return false;out=v->string;return true;}
bool json_uint(const Json *v,uint64_t &out,uint64_t min,uint64_t max) {if(!v||v->type!=Json::Type::integer||v->integer<min||v->integer>max)return false;out=v->integer;return true;}
bool json_bool(const Json *v,bool &out) {if(!v||v->type!=Json::Type::boolean)return false;out=v->boolean;return true;}
FrameReader::Result FrameReader::check_deadline(uint64_t now) {if(failed_)return Result::failed;if(begun_&&!complete_&&(now<last_progress_||now-last_progress_>=progress_timeout_ms||now-started_at_>=frame_timeout_ms)){failed_=true;return Result::timed_out;}return complete_?Result::frame:Result::incomplete;}
FrameReader::Result FrameReader::feed(const uint8_t *bytes,size_t size,size_t &consumed,uint64_t now) {
    consumed=0;auto r=check_deadline(now);if(r!=Result::incomplete)return r;
    if(size&&!bytes){failed_=true;return Result::failed;}
    while(consumed<size&&!complete_) {
        if(!begun_)started_at_=now;
        begun_=true;last_progress_=now;
        if(prefix_size_<4) {prefix_[prefix_size_++]=bytes[consumed++];if(prefix_size_==4){expected_=(uint32_t(prefix_[0])<<24)|(uint32_t(prefix_[1])<<16)|(uint32_t(prefix_[2])<<8)|prefix_[3];if(!expected_||expected_>max_frame_bytes){failed_=true;return Result::invalid_length;}body_.reserve(expected_);}}
        else {const auto n=std::min(size-consumed,expected_-body_.size());body_.append(reinterpret_cast<const char *>(bytes+consumed),n);consumed+=n;complete_=body_.size()==expected_;}
    }
    return complete_?Result::frame:Result::incomplete;
}
void FrameReader::next_frame(){if(complete_)reset();}
void FrameReader::reset(){prefix_size_=expected_=0;body_.clear();begun_=complete_=failed_=false;last_progress_=started_at_=0;}
std::vector<uint8_t> frame(std::string_view body){if(body.empty()||body.size()>max_frame_bytes)return {};const auto n=static_cast<uint32_t>(body.size());std::vector<uint8_t> out{uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)};out.insert(out.end(),body.begin(),body.end());return out;}
} // namespace amity::consortium
