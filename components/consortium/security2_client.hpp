#pragma once
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "psa/crypto.h"
#include <memory>
namespace amity::consortium::platform {
class Security2Client {
public:
    Security2Client(std::string username,std::string password);
    ~Security2Client();
    bool begin(std::string &command);
    bool challenge(std::string_view response,std::string &command);
    bool verify(std::string_view response);
    bool encrypt(std::string_view,std::string &);
    bool decrypt(std::string_view,std::string &);
    bool authenticated()const;
private:
    struct State;std::unique_ptr<State> state_;
};
bool security2_self_test();
} // namespace
#endif
