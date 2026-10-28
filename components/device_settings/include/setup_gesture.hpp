// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace device_settings {
// Released short/medium gestures never fire on the way to a longer hold.
enum class SetupGesture { none, initial_matter, consortium, factory_reset };
class SetupGestureButton {
public:
    static constexpr uint64_t debounce_ms=50,consortium_ms=3000,reset_ms=10000;
    SetupGesture update(bool pressed,uint64_t now){
        if(initialized_&&now<last_now_){*this=SetupGestureButton{};}
        last_now_=now;
        if(!initialized_||pressed!=candidate_){initialized_=true;candidate_=pressed;changed_at_=now;}
        if(now-changed_at_<debounce_ms)return SetupGesture::none;
        if(candidate_!=stable_){
            stable_=candidate_;
            if(stable_){press_edge_at_=changed_at_;pressed_at_=now;fired_=false;}
            else {
                const bool act=armed_&&!fired_;armed_=true;
                if(!act)return SetupGesture::none;
                // A release edge stops the held duration; debounce must not
                // accidentally promote a 9.99s release into factory reset.
                const auto elapsed=changed_at_>=pressed_at_?changed_at_-pressed_at_:0;
                fired_=true;
                if(elapsed>=reset_ms)return SetupGesture::factory_reset;
                // Add counts the accepted physical hold, including its press
                // debounce. Keep the existing factory-reset clock above.
                const auto setup_elapsed=changed_at_>=press_edge_at_?changed_at_-press_edge_at_:0;
                return setup_elapsed>=consortium_ms?SetupGesture::consortium:SetupGesture::initial_matter;
            }
        }
        if(stable_&&candidate_&&armed_&&!fired_&&now-pressed_at_>=reset_ms){fired_=true;return SetupGesture::factory_reset;}
        if(!stable_)armed_=true;
        return SetupGesture::none;
    }
    uint64_t next_check_ms(uint64_t now)const {
        if(!initialized_)return 0;
        if(now-changed_at_<debounce_ms)return debounce_ms-(now-changed_at_);
        if(stable_&&armed_&&!fired_)return now-pressed_at_<reset_ms?reset_ms-(now-pressed_at_):0;
        return UINT64_MAX;
    }
private:
    bool initialized_=false,candidate_=false,stable_=false,armed_=false,fired_=false;
    uint64_t changed_at_=0,press_edge_at_=0,pressed_at_=0,last_now_=0;
};
} // namespace device_settings
