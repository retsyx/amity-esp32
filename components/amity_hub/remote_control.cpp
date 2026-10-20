#include "amity/system_control.hpp"
#include <limits>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <random>
#endif

namespace amity {
namespace {
bool input_random(uint8_t* data, size_t size) {
#ifdef ESP_PLATFORM
    esp_fill_random(data, size);
#else
    std::random_device random; for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(random());
#endif
    return true;
}
}
bool SystemControl::accepts_local_input() const {
    if (!local_coordinator_.load() || maintenance_.load()) return false;
    const auto config = std::atomic_load(&active_config_);
    return !config || (config->controller_octopus_id && *config->controller_octopus_id == octopus_);
}
esp_err_t SystemControl::request_activity(int index) {
    return accepts_local_input() ? local_.request_activity(index) : ESP_ERR_INVALID_STATE;
}
bool SystemControl::press_key(uint8_t key, bool repeat) {
    return accepts_local_input() && local_.press_key(key, repeat);
}
bool SystemControl::release_key() {
    return local_coordinator_.load() && local_.release_key();
}
esp_err_t SystemControl::send_remote_intent(system::InputIntent input, std::optional<system::InputContext> captured) {
    if (!accepts_local_input()) return ESP_ERR_INVALID_STATE;
    input.request_id = system::MessageId::generate(input_random).value_or(system::MessageId{});
    if (!input.request_id) return ESP_ERR_INVALID_STATE;
    const auto context = captured.value_or(local_.input_context());
    input.input_session_id = context.input_session_id; input.expected_state_revision = context.state_revision;
    const auto result = local_.submit_input_intent(std::move(input)); return result ? ESP_OK : ESP_ERR_INVALID_STATE;
}
std::shared_ptr<const SystemControl::RemoteStamp> SystemControl::remote_stamp() const {
    auto config = std::atomic_load(&active_config_); if (!config || !accepts_local_input()) return {};
    const auto snapshot = local_.execution_snapshot();
    if(snapshot.sessions.size()>1)return {};
    const auto zone = snapshot.sessions.empty()?config->primary_zone_id:snapshot.sessions.front().zone_id;
    const auto context = local_.input_context();
    if (!context.input_session_id || context.state_revision != snapshot.state_revision) return {};
    std::string fingerprint = std::to_string(config->config_revision) + ":" + zone.str();
    for(const auto id:config_store::ordered_activities(*config))fingerprint+=":"+id.str();
    for (const auto& session : snapshot.sessions) if (session.zone_id == zone)
        fingerprint += ":" + session.session_id.str() + ":" + std::to_string(session.route_generation) + ":" + session.activity_id.str();
    auto prior = std::atomic_load(&remote_stamp_);
    for (;;) {
        const bool changed = !prior || prior->fingerprint != fingerprint;
        // Exhaustion fails closed. Never wrap a physical event generation.
        const uint32_t generation = !prior ? 1 : changed ? ((!prior->generation || prior->generation == UINT32_MAX) ? 0 : prior->generation + 1) : prior->generation;
        auto next = std::make_shared<const RemoteStamp>(RemoteStamp{fingerprint, generation, context});
        if (std::atomic_compare_exchange_weak(&remote_stamp_, &prior, next)) return next;
    }
}
esp_err_t SystemControl::request_remote_activity(unsigned button) { return request_remote_activity(button, remote_activity_generation()); }
esp_err_t SystemControl::request_remote_activity(unsigned button, uint32_t expected) {
    if (!accepts_local_input()) return ESP_ERR_INVALID_STATE;
    auto config = std::atomic_load(&active_config_); if (!config) return expected == activity_generation() ? request_activity(static_cast<int>(button)) : ESP_ERR_INVALID_STATE;
    auto stamp = remote_stamp(); if (!stamp || !expected || stamp->generation != expected) return ESP_ERR_INVALID_STATE;
    const auto ordered=config_store::ordered_activities(*config);
    if(button>=ordered.size())return ESP_ERR_INVALID_ARG;
    const auto target=std::find_if(config->activities.begin(),config->activities.end(),[&](const auto& a){return a.activity_id==ordered[button]&&a.enabled;});
    if(target==config->activities.end())return ESP_ERR_INVALID_ARG;
    release_remote_key();
    // This runs on the input worker, never a GAP callback or CEC timing task.
    // A cross-screen change first drains the exact reviewed old sessions, with
    // one bounded original deadline. It cannot replay across a concurrent change.
    const auto original=local_.execution_snapshot().sessions;
    const auto deadline=uint64_t(esp_timer_get_time())/1000+1500;
    std::vector<system::ZoneId> stopping;
    for(;;){
        const auto current_config=std::atomic_load(&active_config_);
        if(!accepts_local_input()||!current_config||current_config->config_revision!=config->config_revision||config_store::ordered_activities(*current_config)!=ordered||uint64_t(esp_timer_get_time())/1000>=deadline)return ESP_ERR_INVALID_STATE;
        const auto state=local_.execution_snapshot();
        for(const auto& current:state.sessions)if(std::none_of(original.begin(),original.end(),[&](const auto& prior){return prior.session_id==current.session_id&&prior.route_generation==current.route_generation&&prior.activity_id==current.activity_id&&prior.zone_id==current.zone_id;}))return ESP_ERR_INVALID_STATE;
        bool waiting=bool(state.executing_plan);
        for(const auto& session:state.sessions)if(session.zone_id!=target->zone_id){
            waiting=true;
            if(std::find(stopping.begin(),stopping.end(),session.zone_id)==stopping.end()){
                const auto context=local_.input_context();if(context.state_revision!=state.state_revision)return ESP_ERR_INVALID_STATE;
                system::InputIntent stop;stop.action=system::InputAction::stop_zone;stop.zone_id=session.zone_id;
                stop.input_session_id=context.input_session_id;stop.expected_state_revision=context.state_revision;stop.admissible_until_ms=deadline;
                if(send_remote_intent(std::move(stop),context)!=ESP_OK)return ESP_ERR_INVALID_STATE;
                stopping.push_back(session.zone_id);break;
            }
        }
        if(!waiting){
            const auto context=local_.input_context();if(context.state_revision!=state.state_revision)return ESP_ERR_INVALID_STATE;
            system::InputIntent input;input.action=system::InputAction::select_activity;input.zone_id=target->zone_id;input.activity_id=target->activity_id;input.admissible_until_ms=deadline;
            return send_remote_intent(std::move(input),context);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool SystemControl::press_remote_key(uint8_t key, bool repeat) { return press_remote_key_for_activity(key, remote_activity_generation(), repeat); }
bool SystemControl::press_remote_key_for_activity(uint8_t key, uint32_t expected, bool repeat) {
    if (!accepts_local_input()) return false;
    auto config = std::atomic_load(&active_config_); if (!config) return press_key_for_activity(key, expected, repeat);
    auto stamp = remote_stamp(); if (!stamp || !expected || stamp->generation != expected) return false;
    if (key == 0x40) return stop_remote_zone(expected) == ESP_OK;
    const auto state=local_.execution_snapshot();if(state.sessions.size()>1)return false;
    system::InputIntent input; input.zone_id = state.sessions.empty()?config->primary_zone_id:state.sessions.front().zone_id; input.key = key; input.repeat = repeat;
    auto held = std::atomic_load(&remote_hold_);
    // Physical input is state changes only. The executing owner repeats after
    // each completed wire attempt; an unchanged down never renews or requeues.
    if (repeat && held && held->key == key && held->zone == input.zone_id &&
        held->context.input_session_id == stamp->context.input_session_id &&
        held->context.state_revision == stamp->context.state_revision) return true;
    release_remote_key(); input.action = system::InputAction::key_press;
    input.hold_token = system::HoldToken::generate(input_random).value_or(system::HoldToken{}); if (!input.hold_token) return false;
    const bool accepted = send_remote_intent(input, stamp->context) == ESP_OK;
    if (accepted && repeat) std::atomic_store(&remote_hold_, std::make_shared<const RemoteHold>(RemoteHold{input.hold_token, key, input.zone_id, stamp->context}));
    return accepted;
}
bool SystemControl::release_remote_key() {
    if (!local_coordinator_.load()) { std::atomic_store(&remote_hold_, std::shared_ptr<const RemoteHold>{}); return false; }
    auto config = std::atomic_load(&active_config_); if (!config) return release_key();
    auto held = std::atomic_exchange(&remote_hold_, std::shared_ptr<const RemoteHold>{}); if (!held) return true;
    system::InputIntent input; input.action = system::InputAction::hold_release;
    input.zone_id = held->zone; input.hold_token = held->token;
    return send_remote_intent(std::move(input)) == ESP_OK;
}
esp_err_t SystemControl::stop_remote_zone() { return stop_remote_zone(remote_activity_generation()); }
esp_err_t SystemControl::stop_remote_zone(uint32_t expected) {
    if (!accepts_local_input()) return ESP_ERR_INVALID_STATE;
    auto config = std::atomic_load(&active_config_); if (!config) return expected == activity_generation() ? request_activity(-1) : ESP_ERR_INVALID_STATE;
    auto stamp = remote_stamp(); if (!stamp || !expected || stamp->generation != expected) return ESP_ERR_INVALID_STATE;
    release_remote_key(); system::InputIntent input; input.action = system::InputAction::stop_zone;
    const auto state=local_.execution_snapshot();input.zone_id=state.sessions.empty()?config->primary_zone_id:state.sessions.front().zone_id; return send_remote_intent(std::move(input), stamp->context);
}
uint32_t SystemControl::remote_activity_generation() const {
    if (!accepts_local_input()) return 0;
    if (!std::atomic_load(&active_config_)) return activity_generation();
    auto stamp = remote_stamp(); return stamp ? stamp->generation : 0;
}
int SystemControl::remote_activity_target() const {
    if (!accepts_local_input()) return -1;
    const auto config=std::atomic_load(&active_config_);if(!config)return activity_target();
    const auto state=local_.execution_snapshot();if(state.sessions.size()!=1)return -1;
    const auto ordered=config_store::ordered_activities(*config);
    const auto found=std::find(ordered.begin(),ordered.end(),state.sessions.front().activity_id);
    return found==ordered.end()?-1:static_cast<int>(found-ordered.begin());
}
}
