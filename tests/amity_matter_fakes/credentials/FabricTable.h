/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The fabric table surface Amity uses. Its members are strict seams: each
// scenario scripts the table it needs. FabricInfo holds only an index and a
// vendor; its two-argument constructor exists only in the fake.
#include "lib/core/CHIPError.h"
#include <cstdint>

// CONFIG_MAX_FABRICS in the Amity build.
#define CHIP_CONFIG_MAX_FABRICS 5

namespace chip {

using FabricIndex = uint8_t;
inline constexpr FabricIndex kUndefinedFabricIndex = 0;
inline constexpr FabricIndex kMinValidFabricIndex = 1;
inline constexpr FabricIndex kMaxValidFabricIndex = UINT8_MAX - 1;

enum class VendorId : uint16_t {
    Common = 0x0000u,
    NotSpecified = 0xFFFFu,
};

class FabricInfo {
public:
    FabricInfo() = default;
    FabricInfo(FabricIndex index, VendorId vendor) : mFabricIndex(index), mVendorId(vendor) {}

    FabricIndex GetFabricIndex() const { return mFabricIndex; }
    VendorId GetVendorId() const { return mVendorId; }

private:
    FabricIndex mFabricIndex = kUndefinedFabricIndex;
    VendorId mVendorId = VendorId::NotSpecified;
};

class FabricTable {
public:
    class Delegate {
    public:
        Delegate() {}
        virtual ~Delegate() {}
        virtual void FabricWillBeRemoved(const FabricTable &, FabricIndex) {}
        virtual void OnFabricRemoved(const FabricTable &, FabricIndex) {}
        virtual void OnFabricCommitted(const FabricTable &, FabricIndex) {}
        virtual void OnFabricUpdated(const FabricTable &, FabricIndex) {}
        Delegate *next = nullptr;
    };

    FabricTable() = default;
    FabricTable(const FabricTable &) = delete;
    void operator=(const FabricTable &) = delete;

    CHIP_ERROR Delete(FabricIndex fabricIndex);
    const FabricInfo *FindFabricWithIndex(FabricIndex fabricIndex) const;
    CHIP_ERROR AddFabricDelegate(Delegate *delegate);
    void RemoveFabricDelegate(Delegate *delegate);
    // Iterates the committed and pending fabrics, as the SDK's iterator does.
    const FabricInfo *begin() const;
    const FabricInfo *end() const;
    FabricIndex GetPendingNewFabricIndex() const;
};

}  // namespace chip
