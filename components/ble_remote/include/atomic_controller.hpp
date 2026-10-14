/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */
#pragma once
#include "remote_input.hpp"

namespace ble {
// Physical HID translation only; the shared controller owns all button policy.
class AtomicInputAdapter {
public:
    explicit AtomicInputAdapter(RemoteInputController &controller) : controller_(controller) {}
    void input(uint16_t page, uint16_t usage, bool pressed, InputStamp stamp);
private:
    RemoteInputController &controller_;
};
} // namespace ble
