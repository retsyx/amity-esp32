/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The Matter server's fabric table and fail-safe. The fail-safe query is a
// strict seam.
#include "credentials/FabricTable.h"

namespace chip {
namespace app {

class FailSafeContext {
public:
    bool IsFailSafeFullyDisarmed() const;
};

}  // namespace app

class Server {
public:
    static Server &GetInstance();
    FabricTable &GetFabricTable() { return mFabrics; }
    app::FailSafeContext &GetFailSafeContext() { return mFailSafeContext; }

private:
    FabricTable mFabrics;
    app::FailSafeContext mFailSafeContext;
};

}  // namespace chip
