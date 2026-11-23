/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Strict seams: the one definition of a function that a test binary calls
// outside the firmware files it builds. A seam records each call by name and
// runs the behaviour the scenario installed. A call without one fails the
// scenario, so no test passes through a dependency it did not script.
//
//   esp_err_t web::stop(httpd_handle_t handle) { return AMITY_SEAM(web::stop, handle); }
//
//   amity_seam::install<&web::stop>([](httpd_handle_t) { return ESP_OK; });
//   CHECK(amity_seam::count("web::stop") == 1);
//
// A member function's behaviour takes the object first:
// install<&C::f>([](C &self, ...) {...}). An overloaded function names its
// type: AMITY_SEAM_AS(void (*)(uint32_t), reboot_later, delay). A constructor
// that decides part of the object's state calls the key constructor<R, C,
// Args...>, whose behaviour takes the new object first and returns that state;
// other constructors and destructors only record their call. Install
// behaviours before starting threads that call the seams; the call log may be
// read from any thread.
#include "scenario_main.hpp"
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amity_seam {

template <typename F> struct Signature;
template <typename R, typename... A> struct Signature<R (*)(A...)> {
    using Behaviour = std::function<R(A...)>;
};
template <typename R, typename... A> struct Signature<R (*)(A...) noexcept> {
    using Behaviour = std::function<R(A...)>;
};
template <typename R, typename C, typename... A> struct Signature<R (C::*)(A...)> {
    using Behaviour = std::function<R(C &, A...)>;
};
template <typename R, typename C, typename... A> struct Signature<R (C::*)(A...) noexcept> {
    using Behaviour = std::function<R(C &, A...)>;
};
template <typename R, typename C, typename... A> struct Signature<R (C::*)(A...) const> {
    using Behaviour = std::function<R(const C &, A...)>;
};
template <typename R, typename C, typename... A> struct Signature<R (C::*)(A...) const noexcept> {
    using Behaviour = std::function<R(const C &, A...)>;
};

template <auto F> using Behaviour = typename Signature<decltype(F)>::Behaviour;

// The behaviour installed for F, empty until a scenario installs one.
template <auto F> Behaviour<F> &behaviour() {
    static Behaviour<F> installed;
    return installed;
}
template <auto F, typename B> void install(B &&value) { behaviour<F>() = std::forward<B>(value); }
template <auto F> void uninstall() { behaviour<F>() = nullptr; }

// The key for a constructor seam, which has no address of its own.
template <typename R, typename C, typename... A> R constructor(C &, A...) { return R(); }

struct Journal {
    std::mutex mutex;
    std::vector<std::string> calls;
};
inline Journal &journal() {
    static Journal instance;
    return instance;
}
inline void record(const char *name) {
    std::lock_guard<std::mutex> lock(journal().mutex);
    journal().calls.emplace_back(name);
}
// The seams called so far, in call order.
inline std::vector<std::string> calls() {
    std::lock_guard<std::mutex> lock(journal().mutex);
    return journal().calls;
}
inline std::size_t count(std::string_view name) {
    std::lock_guard<std::mutex> lock(journal().mutex);
    std::size_t found = 0;
    for (const auto &call : journal().calls) found += call == name;
    return found;
}
inline void clear_calls() {
    std::lock_guard<std::mutex> lock(journal().mutex);
    journal().calls.clear();
}

template <auto F, typename... A> decltype(auto) call(const char *name, A &&...arguments) {
    record(name);
    auto &installed = behaviour<F>();
    if (!installed) {
        const std::string message = std::string("seam ") + name + " was called without a behaviour";
        amity_test::fail(__FILE__, __LINE__, message.c_str());
    }
    return installed(std::forward<A>(arguments)...);
}

}  // namespace amity_seam

#define AMITY_SEAM(function, ...) ::amity_seam::call<&function>(#function, ##__VA_ARGS__)
#define AMITY_SEAM_AS(type, function, ...) ::amity_seam::call<static_cast<type>(&function)>(#function, ##__VA_ARGS__)
