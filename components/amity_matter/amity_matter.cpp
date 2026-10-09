/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity_matter.h"
#include "commissioning_credentials.h"
#include "diagnostics.h"
#include "apple_fabrics.h"
#include "device_settings.hpp"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi_default.h"

#include <cstring>
#include <utility>
#include "esp_log.h"
#include <esp_matter.h>
#include <app/AttributeAccessInterface.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/CommandHandler.h>
#include <app/server/Dnssd.h>
#include <app/clusters/keypad-input-server/keypad-input-delegate.h>
#include <app/clusters/media-input-server/media-input-delegate.h>
#include <app/clusters/media-playback-server/media-playback-delegate.h>
#include <app/reporting/reporting.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>

namespace {
using namespace chip::app::Clusters;
using chip::app::CommandResponseHelper;
using chip::Protocols::InteractionModel::Status;
constexpr uint16_t player_endpoint = 1;
constexpr uint16_t speaker_endpoint = 2;
constexpr uint16_t remote_battery_endpoint = 3;
constexpr const char *tag = "amity_matter";

amity_matter_callbacks_t callbacks{};
amity_matter_state_t state{};
amity_matter_input_t inputs[AMITY_MATTER_MAX_INPUTS]{};
size_t input_count = 0;
bool started = false;
// CEC does not expose reliable transport feedback. This is the last accepted
// playback command, never a claim that a source acknowledged a playback state.
MediaPlayback::PlaybackStateEnum requested_playback = MediaPlayback::PlaybackStateEnum::kNotPlaying;

esp_err_t result(CHIP_ERROR error)
{
    if (error == CHIP_NO_ERROR) return ESP_OK;
    ESP_LOGW(tag, "Matter operation failed: %" CHIP_ERROR_FORMAT, error.Format());
    return ESP_FAIL;
}

void report(uint16_t endpoint, uint32_t cluster, uint32_t attribute)
{
    MatterReportingAttributeChangeCallback(endpoint, cluster, attribute);
}

esp_err_t dispatch_key(uint8_t key)
{
    return callbacks.key ? callbacks.key(key, callbacks.context) : ESP_ERR_NOT_SUPPORTED;
}

class Inputs final : public MediaInput::Delegate {
public:
    CHIP_ERROR HandleGetInputList(chip::app::AttributeValueEncoder &encoder) override
    {
        return encoder.EncodeList([](const auto &list) -> CHIP_ERROR {
            for (size_t i = 0; i < input_count; ++i) {
                MediaInput::Structs::InputInfoStruct::Type entry;
                entry.index = inputs[i].index;
                entry.inputType = MediaInput::InputTypeEnum::kHdmi;
                entry.name = chip::CharSpan::fromCharString(inputs[i].name);
                entry.description = chip::CharSpan::fromCharString("Amity activity");
                ReturnErrorOnFailure(list.Encode(entry));
            }
            return CHIP_NO_ERROR;
        });
    }
    uint8_t HandleGetCurrentInput() override { return state.current_input; }
    bool HandleSelectInput(uint8_t index) override
    {
        for (size_t i = 0; i < input_count; ++i) {
            if (inputs[i].index == index)
                return callbacks.activity && callbacks.activity(index, callbacks.context) == ESP_OK;
        }
        return false;
    }
    bool HandleShowInputStatus() override { return false; }
    bool HandleHideInputStatus() override { return false; }
    bool HandleRenameInput(uint8_t, const chip::CharSpan &) override { return false; }
} input_delegate;

using PlaybackResponse = CommandResponseHelper<MediaPlayback::Commands::PlaybackResponse::Type>;
class Playback final : public MediaPlayback::Delegate {
    void respond(PlaybackResponse &helper, uint8_t key, MediaPlayback::PlaybackStateEnum next)
    {
        MediaPlayback::Commands::PlaybackResponse::Type response;
        const esp_err_t err = dispatch_key(key);
        response.status = err == ESP_OK ? MediaPlayback::StatusEnum::kSuccess
                                       : MediaPlayback::StatusEnum::kInvalidStateForCommand;
        if (err == ESP_OK) {
            requested_playback = next;
            report(player_endpoint, MediaPlayback::Id, MediaPlayback::Attributes::CurrentState::Id);
        }
        (void)result(helper.Success(response));
    }
    void unsupported(PlaybackResponse &helper)
    {
        MediaPlayback::Commands::PlaybackResponse::Type response;
        response.status = MediaPlayback::StatusEnum::kNotAllowed;
        (void)result(helper.Success(response));
    }
public:
    MediaPlayback::PlaybackStateEnum HandleGetCurrentState() override { return requested_playback; }
    uint64_t HandleGetStartTime() override { return 0; }
    uint64_t HandleGetDuration() override { return 0; }
    CHIP_ERROR HandleGetSampledPosition(chip::app::AttributeValueEncoder &) override { return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE; }
    float HandleGetPlaybackSpeed() override { return 0; }
    uint64_t HandleGetSeekRangeStart() override { return 0; }
    uint64_t HandleGetSeekRangeEnd() override { return 0; }
    CHIP_ERROR HandleGetActiveAudioTrack(chip::app::AttributeValueEncoder &) override { return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE; }
    CHIP_ERROR HandleGetAvailableAudioTracks(chip::app::AttributeValueEncoder &e) override { return e.EncodeEmptyList(); }
    CHIP_ERROR HandleGetActiveTextTrack(chip::app::AttributeValueEncoder &) override { return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE; }
    CHIP_ERROR HandleGetAvailableTextTracks(chip::app::AttributeValueEncoder &e) override { return e.EncodeEmptyList(); }
    void HandlePlay(PlaybackResponse &h) override { respond(h, 0x44, MediaPlayback::PlaybackStateEnum::kPlaying); }
    void HandlePause(PlaybackResponse &h) override { respond(h, 0x46, MediaPlayback::PlaybackStateEnum::kPaused); }
    void HandleStop(PlaybackResponse &h) override { respond(h, 0x45, MediaPlayback::PlaybackStateEnum::kNotPlaying); }
    void HandlePrevious(PlaybackResponse &h) override { respond(h, 0x4c, requested_playback); }
    void HandleNext(PlaybackResponse &h) override { respond(h, 0x4b, requested_playback); }
    void HandleFastForward(PlaybackResponse &h, const chip::Optional<bool> &) override { unsupported(h); }
    void HandleRewind(PlaybackResponse &h, const chip::Optional<bool> &) override { unsupported(h); }
    void HandleSkipBackward(PlaybackResponse &h, const uint64_t &) override { unsupported(h); }
    void HandleSkipForward(PlaybackResponse &h, const uint64_t &) override { unsupported(h); }
    void HandleSeek(PlaybackResponse &h, const uint64_t &) override { unsupported(h); }
    void HandleStartOver(PlaybackResponse &h) override { unsupported(h); }
    bool HandleActivateAudioTrack(const chip::CharSpan &, const uint8_t &) override { return false; }
    bool HandleActivateTextTrack(const chip::CharSpan &) override { return false; }
    bool HandleDeactivateTextTrack() override { return false; }
    uint32_t GetFeatureMap(chip::EndpointId) override { return 0; }
    uint16_t GetClusterRevision(chip::EndpointId) override { return 2; }
} playback_delegate;

class Keypad final : public KeypadInput::Delegate {
public:
    void HandleSendKey(CommandResponseHelper<KeypadInput::Commands::SendKeyResponse::Type> &helper,
                       const KeypadInput::CECKeyCodeEnum &code) override
    {
        const uint8_t key = static_cast<uint8_t>(code);
        const bool supported = key <= 0x0d || key == 0x10 || key == 0x11 || (key >= 0x20 && key <= 0x29);
        KeypadInput::Commands::SendKeyResponse::Type response;
        response.status = !supported ? KeypadInput::StatusEnum::kUnsupportedKey
                          : dispatch_key(key) == ESP_OK ? KeypadInput::StatusEnum::kSuccess
                                                       : KeypadInput::StatusEnum::kInvalidKeyInCurrentState;
        (void)result(helper.Success(response));
    }
    uint32_t GetFeatureMap(chip::EndpointId) override { return 7; }
} keypad_delegate;

// Use CHIP's access registry: the installed ESP Matter generated provider does
// not dispatch the older esp_matter attribute override API. OnOff cannot encode
// null, so unknown mute is a read failure instead of a fabricated boolean.
class ObservedOnOff final : public chip::app::AttributeAccessInterface {
public:
    ObservedOnOff() : AttributeAccessInterface(chip::Optional<chip::EndpointId>::Missing(), OnOff::Id) {}
    CHIP_ERROR Read(const chip::app::ConcreteReadAttributePath &path, chip::app::AttributeValueEncoder &encoder) override
    {
        if (path.mAttributeId != OnOff::Attributes::OnOff::Id) return CHIP_NO_ERROR;
        if (path.mEndpointId == player_endpoint) return encoder.Encode(state.power);
        if (path.mEndpointId != speaker_endpoint) return CHIP_NO_ERROR;
        if (!state.mute_known) return CHIP_ERROR_IM_GLOBAL_STATUS_VALUE(Status::Failure);
        return encoder.Encode(!state.muted);
    }
} observed_onoff;

class ObservedVolume final : public chip::app::AttributeAccessInterface {
public:
    ObservedVolume() : AttributeAccessInterface(chip::MakeOptional<chip::EndpointId>(speaker_endpoint), LevelControl::Id) {}
    CHIP_ERROR Read(const chip::app::ConcreteReadAttributePath &path, chip::app::AttributeValueEncoder &encoder) override
    {
        if (path.mAttributeId != LevelControl::Attributes::CurrentLevel::Id) return CHIP_NO_ERROR;
        return state.volume_known ? encoder.Encode(state.volume) : encoder.EncodeNull();
    }
} observed_volume;

template <class Command>
bool valid_command(chip::TLV::TLVReader &reader)
{
    Command decoded;
    return chip::app::DataModel::Decode(reader, decoded) == CHIP_NO_ERROR;
}

// A user callback runs before the generated command callback. Sending the
// response here and returning non-OK deliberately suppresses the SDK's virtual
// on/off/level state machine; the hub publishes the actual state separately.
esp_err_t on_off_command(const chip::app::ConcreteCommandPath &path, chip::TLV::TLVReader &reader, void *opaque)
{
    auto *handler = static_cast<chip::app::CommandHandler *>(opaque);
    if (!handler) return ESP_FAIL;
    bool on = false;
    bool valid = false;
    if (path.mCommandId == OnOff::Commands::On::Id) {
        valid = valid_command<OnOff::Commands::On::DecodableType>(reader);
        on = true;
    } else if (path.mCommandId == OnOff::Commands::Off::Id) {
        valid = valid_command<OnOff::Commands::Off::DecodableType>(reader);
    } else if (path.mCommandId == OnOff::Commands::Toggle::Id) {
        valid = valid_command<OnOff::Commands::Toggle::DecodableType>(reader);
        if (path.mEndpointId == speaker_endpoint && !state.mute_known) {
            handler->AddStatus(path, Status::InvalidInState);
            return ESP_FAIL;
        }
        on = (path.mEndpointId == player_endpoint) ? !state.power : state.muted;
    } else {
        handler->AddStatus(path, Status::UnsupportedCommand);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (valid && (path.mEndpointId == player_endpoint) && callbacks.power)
        err = callbacks.power(on, callbacks.context);
    else if (valid && path.mEndpointId == speaker_endpoint && callbacks.mute)
        err = callbacks.mute(!on, callbacks.context);
    handler->AddStatus(path, !valid ? Status::InvalidCommand : err == ESP_OK ? Status::Success : Status::Failure);
    return ESP_FAIL;
}

template <class Command>
Status step_command(chip::TLV::TLVReader &reader)
{
    Command command;
    if (chip::app::DataModel::Decode(reader, command) != CHIP_NO_ERROR) return Status::InvalidCommand;
    if (command.stepMode != LevelControl::StepModeEnum::kUp && command.stepMode != LevelControl::StepModeEnum::kDown)
        return Status::ConstraintError;
    if (!command.stepSize) return Status::Success;
    const int16_t steps = command.stepMode == LevelControl::StepModeEnum::kUp ? command.stepSize : -int16_t(command.stepSize);
    return callbacks.volume_step && callbacks.volume_step(steps, callbacks.context) == ESP_OK ? Status::Success : Status::Failure;
}

esp_err_t level_command(const chip::app::ConcreteCommandPath &path, chip::TLV::TLVReader &reader, void *opaque)
{
    auto *handler = static_cast<chip::app::CommandHandler *>(opaque);
    if (!handler) return ESP_FAIL;
    Status status = Status::UnsupportedCommand;
    switch (path.mCommandId) {
    case LevelControl::Commands::Step::Id:
        status = step_command<LevelControl::Commands::Step::DecodableType>(reader);
        break;
    default:
        // Relative CEC keypresses cannot implement an absolute level, continuous
        // rate, or a volume-linked mute state. Only ordinary Step is supported.
        break;
    }
    handler->AddStatus(path, status);
    return ESP_FAIL;
}

esp_err_t attribute_changed(esp_matter::attribute::callback_type_t, uint16_t, uint32_t,
                            uint32_t, esp_matter_attr_val_t *, void *)
{
    return ESP_OK;
}

void event_callback(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    using namespace chip::DeviceLayer;
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(tag, "Commissioning complete; fabric persisted");
        break;
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(tag, "Commissioning window opened");
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(tag, "Commissioning window closed");
        break;
    case DeviceEventType::kFabricRemoved:
        ESP_LOGI(tag, "Matter fabric removed; Amity configuration retained");
        break;
    default:
        break;
    }
}

esp_err_t install_command_handlers(esp_matter::endpoint_t *endpoint, bool speaker)
{
    using namespace esp_matter;
    auto *onoff = cluster::get(endpoint, OnOff::Id);
    if (!onoff) return ESP_FAIL;
    for (auto *command = command::get_first(onoff); command; command = command::get_next(command))
        command::set_user_callback(command, on_off_command);
    if (speaker) {
        auto *level = cluster::get(endpoint, LevelControl::Id);
        if (!level) return ESP_FAIL;
        for (auto *command = command::get_first(level); command; command = command::get_next(command))
            command::set_user_callback(command, level_command);
    }
    return ESP_OK;
}
}

extern "C" esp_err_t amity_matter_start(const amity_matter_callbacks_t *app_callbacks)
{
    if (!app_callbacks) return ESP_ERR_INVALID_ARG;
    if (started) return ESP_ERR_INVALID_STATE;
    callbacks = *app_callbacks;
    esp_err_t err = amity_matter_prepare_credentials();
    if (err != ESP_OK) return err;

    using namespace esp_matter;
    node::config_t node_config;
    strlcpy(node_config.root_node.basic_information.node_label, "Amity",
            sizeof(node_config.root_node.basic_information.node_label));
    auto *node = node::create(&node_config, attribute_changed, nullptr);
    if (!node) return ESP_ERR_NO_MEM;
    err = amity_diagnostics::configure(node);
    if (err != ESP_OK) return err;

    endpoint::basic_video_player::config_t player_config;
    player_config.media_playback.delegate = &playback_delegate;
    player_config.keypad_input.delegate = &keypad_delegate;
    auto *player = endpoint::basic_video_player::create(node, &player_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!player || endpoint::get_id(player) != player_endpoint) return ESP_FAIL;
    cluster::media_input::config_t input_config;
    input_config.delegate = &input_delegate;
    if (!cluster::media_input::create(player, &input_config, CLUSTER_FLAG_SERVER)) return ESP_ERR_NO_MEM;
    auto *playback = cluster::get(player, MediaPlayback::Id);
    if (!cluster::media_playback::command::create_previous(playback) ||
        !cluster::media_playback::command::create_next(playback)) return ESP_ERR_NO_MEM;
    auto *keypad = cluster::get(player, KeypadInput::Id);
    if (cluster::keypad_input::feature::navigation_key_codes::add(keypad) != ESP_OK ||
        cluster::keypad_input::feature::location_keys::add(keypad) != ESP_OK ||
        cluster::keypad_input::feature::number_keys::add(keypad) != ESP_OK) return ESP_FAIL;

    endpoint::speaker::config_t speaker_config;
    speaker_config.level_control.min_level = 0;
    auto *speaker = endpoint::speaker::create(node, &speaker_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!speaker || endpoint::get_id(speaker) != speaker_endpoint) return ESP_FAIL;
    endpoint::power_source::config_t battery_config;
    auto &battery = battery_config.power_source;
    strlcpy(battery.description, "Remote battery", sizeof(battery.description));
    battery.feature_flags = cluster::power_source::feature::battery::get_id() |
                            cluster::power_source::feature::rechargeable::get_id();
    battery.features.rechargeable.bat_charge_state = static_cast<uint8_t>(PowerSource::BatChargeStateEnum::kIsNotCharging);
    battery.features.rechargeable.bat_functional_while_charging = true;
    auto *remote_battery = endpoint::power_source::create(node, &battery_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!remote_battery || endpoint::get_id(remote_battery) != remote_battery_endpoint) return ESP_FAIL;
    auto *battery_cluster = cluster::get(remote_battery, PowerSource::Id);
    if (!cluster::power_source::attribute::create_bat_percent_remaining(battery_cluster, nullable<uint8_t>(200)))
        return ESP_ERR_NO_MEM;
    // Its EndpointList stays empty: this battery belongs to the remote, not the ESP32/player/speaker.
    err = install_command_handlers(player, false);
    if (err != ESP_OK) return err;
    err = install_command_handlers(speaker, true);
    if (err != ESP_OK) return err;
    // Create the station before Matter starts Wi-Fi so the first DHCP request
    // already carries the provisioned hostname. Matter reuses this interface.
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_ERR_NO_MEM;
    err = esp_netif_set_hostname(netif, device_settings::hostname());
    if (err != ESP_OK) return err;
    err = esp_matter::start(event_callback);
    if (err != ESP_OK) return err;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        err = amity_matter::start_apple_fabric_cleanup();
        if (err != ESP_OK) return err;
        // A commissioned boot can return before WiFi has an address. Initialize
        // Matter's shared mDNS hostname now so the HTTPS alias can be registered
        // safely; the SDK refreshes the records when connectivity arrives.
        chip::app::DnssdServer::Instance().StartServer();
        auto &registry = chip::app::AttributeAccessInterfaceRegistry::Instance();
        if (!registry.Register(&observed_onoff) || !registry.Register(&observed_volume)) return ESP_ERR_INVALID_STATE;
    }
    started = true;
    ESP_LOGI(tag, "Matter ready: video player endpoint 1, speaker endpoint 2, remote battery endpoint 3");
    return ESP_OK;
}

extern "C" esp_err_t amity_matter_set_inputs(const amity_matter_input_t *new_inputs, size_t count)
{
    if (count > AMITY_MATTER_MAX_INPUTS || (count && !new_inputs)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) {
        if (!memchr(new_inputs[i].name, '\0', sizeof(new_inputs[i].name)) || !new_inputs[i].name[0])
            return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j)
            if (new_inputs[i].index == new_inputs[j].index) return ESP_ERR_INVALID_ARG;
    }
    // Before start, there is no CHIP task or CHIP lock yet.
    auto copy = [&] {
        if (count) memcpy(inputs, new_inputs, count * sizeof(*inputs));
        input_count = count;
    };
    if (started) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        copy();
        report(player_endpoint, MediaInput::Id, MediaInput::Attributes::InputList::Id);
    } else {
        copy();
    }
    return ESP_OK;
}

extern "C" esp_err_t amity_matter_update_state(const amity_matter_state_t *next)
{
    if (!next || (next->volume_known && next->volume > 254)) return ESP_ERR_INVALID_ARG;
    if (!started) {
        state = *next;
        return ESP_OK;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    const auto old = state;
    state = *next;
    if (old.power != state.power) {
        report(player_endpoint, OnOff::Id, OnOff::Attributes::OnOff::Id);
    }
    if (old.current_input != state.current_input)
        report(player_endpoint, MediaInput::Id, MediaInput::Attributes::CurrentInput::Id);
    if (old.mute_known != state.mute_known || old.muted != state.muted)
        report(speaker_endpoint, OnOff::Id, OnOff::Attributes::OnOff::Id);
    if (old.volume_known != state.volume_known || old.volume != state.volume)
        report(speaker_endpoint, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    return ESP_OK;
}

extern "C" esp_err_t amity_matter_update_remote(bool paired, bool connected, int percent, int charging)
{
    if (percent < -1 || percent > 100 || charging < -1 || charging > 1) return ESP_ERR_INVALID_ARG;
    if (!started) return ESP_ERR_INVALID_STATE;
    if (!paired) { percent = 100; charging = 0; }
    else if (!connected) { percent = charging = -1; }
    using namespace esp_matter;
    using namespace PowerSource;
    const auto status = !paired ? PowerSourceStatusEnum::kUnspecified :
        connected ? PowerSourceStatusEnum::kActive : PowerSourceStatusEnum::kUnavailable;
    const auto charge = charging < 0 ? BatChargeStateEnum::kUnknown :
        charging ? BatChargeStateEnum::kIsCharging : BatChargeStateEnum::kIsNotCharging;
    // Mandatory coarse charge level uses warning <=20%, critical <=10%.
    const auto level = percent < 0 || percent > 20 ? BatChargeLevelEnum::kOk :
        percent > 10 ? BatChargeLevelEnum::kWarning : BatChargeLevelEnum::kCritical;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    static bool valid = false;
    static int old_percent, old_charge, old_status;
    if (valid && old_percent == percent && old_charge == static_cast<int>(charge) && old_status == static_cast<int>(status)) return ESP_OK;
    auto percent_value = esp_matter_nullable_uint8(percent < 0 ? nullable<uint8_t>() : nullable<uint8_t>(percent * 2));
    auto charge_value = esp_matter_enum8(static_cast<uint8_t>(charge));
    auto status_value = esp_matter_enum8(static_cast<uint8_t>(status));
    auto level_value = esp_matter_enum8(static_cast<uint8_t>(level));
    for (auto item : {std::pair<uint32_t, esp_matter_attr_val_t *>{Attributes::BatPercentRemaining::Id, &percent_value},
                      {Attributes::BatChargeState::Id, &charge_value}, {Attributes::Status::Id, &status_value},
                      {Attributes::BatChargeLevel::Id, &level_value}}) {
        auto err = attribute::update(remote_battery_endpoint, PowerSource::Id, item.first, item.second);
        if (err != ESP_OK) return err;
    }
    valid = true; old_percent = percent; old_charge = static_cast<int>(charge); old_status = static_cast<int>(status);
    return ESP_OK;
}

extern "C" esp_err_t amity_matter_get_status(amity_matter_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->started = started;
    if (!started) return ESP_OK;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &server = chip::Server::GetInstance();
    status->commissioning_window = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
    for (const auto &fabric : server.GetFabricTable()) {
        if (status->fabric_count >= AMITY_MATTER_MAX_FABRICS) break;
        auto &entry = status->fabrics[status->fabric_count++];
        entry.index = fabric.GetFabricIndex();
        entry.vendor_id = static_cast<uint16_t>(fabric.GetVendorId());
        entry.fabric_id = fabric.GetFabricId();
        auto label = fabric.GetFabricLabel();
        const size_t length = label.size() < sizeof(entry.label) - 1 ? label.size() : sizeof(entry.label) - 1;
        memcpy(entry.label, label.data(), length);
        entry.label[length] = '\0';
    }
    const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);
    chip::MutableCharSpan manual(status->manual_code, sizeof(status->manual_code) - 1);
    chip::MutableCharSpan qr(status->qr_payload, sizeof(status->qr_payload) - 1);
    const CHIP_ERROR manual_err = GetManualPairingCode(manual, flags);
    const CHIP_ERROR qr_err = GetQRCode(qr, flags);
    if (manual_err == CHIP_NO_ERROR) status->manual_code[manual.size()] = '\0';
    if (qr_err == CHIP_NO_ERROR) status->qr_payload[qr.size()] = '\0';
    return result(manual_err != CHIP_NO_ERROR ? manual_err : qr_err);
}

extern "C" esp_err_t amity_matter_open_commissioning(uint16_t seconds)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    if (seconds < 180 || seconds > 900) return ESP_ERR_INVALID_ARG;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (manager.IsCommissioningWindowOpen()) return ESP_OK;
    return result(manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(seconds),
                                                       chip::CommissioningWindowAdvertisement::kAllSupported));
}

extern "C" esp_err_t amity_matter_close_commissioning(void)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    chip::Server::GetInstance().GetCommissioningWindowManager().CloseCommissioningWindow();
    return ESP_OK;
}

extern "C" esp_err_t amity_matter_remove_fabric(uint8_t index)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    if (!index) return ESP_ERR_INVALID_ARG;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &table = chip::Server::GetInstance().GetFabricTable();
    if (!table.FindFabricWithIndex(index)) return ESP_ERR_NOT_FOUND;
    return result(table.Delete(index));
}
