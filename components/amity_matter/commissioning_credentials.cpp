/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "commissioning_credentials.h"

#include <cstring>
#include "device_settings.hpp"
#include "esp_log.h"
#include <crypto/CHIPCryptoPAL.h>
#include <esp_matter_providers.h>
#include <platform/CommissionableDataProvider.h>

namespace {
constexpr uint32_t iterations = 1000;
struct CommissioningCredentials {
    uint32_t passcode;
    uint16_t discriminator;
    uint8_t salt[32];
};

class Credentials final : public chip::DeviceLayer::CommissionableDataProvider {
public:
    CommissioningCredentials stored{};

    CHIP_ERROR GetSetupDiscriminator(uint16_t &value) override {
        value = stored.discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &value) override {
        value = iterations;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &value) override {
        return chip::CopySpanToMutableSpan(chip::ByteSpan(stored.salt), value);
    }
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &value, size_t &length) override {
        length = chip::Crypto::kSpake2p_VerifierSerialized_Length;
        if (value.size() < length) return CHIP_ERROR_BUFFER_TOO_SMALL;
        chip::Crypto::Spake2pVerifier verifier;
        ReturnErrorOnFailure(verifier.Generate(iterations, chip::ByteSpan(stored.salt), stored.passcode));
        return verifier.Serialize(value);
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &value) override {
        value = stored.passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
} credentials;
}

esp_err_t amity_matter_prepare_credentials()
{
    const auto *factory = device_settings::factory();
    if (!factory) {
        ESP_LOGE("amity_matter", "No factory provisioning credentials; install an amity-provision image");
        return ESP_ERR_INVALID_STATE;
    }
    credentials.stored.passcode = factory->passcode;
    credentials.stored.discriminator = factory->discriminator;
    memcpy(credentials.stored.salt, factory->salt, sizeof(factory->salt));
    esp_matter::set_custom_commissionable_data_provider(&credentials);
    ESP_LOGI("amity_matter", "Using factory-provisioned commissioning credentials");
    return ESP_OK;
}
