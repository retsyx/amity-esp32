// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mutex>
#include <utility>

namespace amity::consortium {
// The caller holds the persistence mutex before acquiring this runtime lock.
// Keep writers serialized through capture/save/publish, while readers and
// independent owners can use the runtime during a synchronous flash operation.
template<class Save>
auto without_runtime_lock(std::unique_lock<std::mutex>& runtime,Save&& save) {
    struct Unlocked {
        std::unique_lock<std::mutex>& guard;
        explicit Unlocked(std::unique_lock<std::mutex>& value):guard(value){guard.unlock();}
        ~Unlocked(){guard.lock();}
    } unlocked(runtime);
    return std::forward<Save>(save)();
}
}
