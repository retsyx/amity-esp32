/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The part of CHIP's non-owning Span the Amity sources use.
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chip {

template <class T> class Span {
public:
    using pointer = T *;

    constexpr Span() = default;
    constexpr Span(pointer databuf, size_t datalen) : mDataBuf(databuf), mDataLen(datalen) {}
    template <class U, size_t N,
              typename = std::enable_if_t<sizeof(U) == sizeof(T) && std::is_convertible<U *, T *>::value>>
    constexpr explicit Span(U (&databuf)[N]) : Span(databuf, N) {}

    constexpr pointer data() const { return mDataBuf; }
    constexpr size_t size() const { return mDataLen; }
    constexpr bool empty() const { return size() == 0; }
    constexpr pointer begin() const { return data(); }
    constexpr pointer end() const { return data() + size(); }

private:
    pointer mDataBuf = nullptr;
    size_t mDataLen = 0;
};

using ByteSpan = Span<const uint8_t>;
using MutableByteSpan = Span<uint8_t>;
using CharSpan = Span<const char>;
using MutableCharSpan = Span<char>;

}  // namespace chip
