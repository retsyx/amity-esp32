/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Checks tests/scenario_main.hpp. CTest expects no-checks and failed-check to fail.
#include "scenario_main.hpp"
#include <thread>
#include <vector>

namespace {

constexpr unsigned threads_used = 4, checks_per_thread = 1000;

void checks() {
    const int value = 2;
    CHECK(value == 2);
    CHECK(value + value == 4);
}

// Fakes may CHECK on their own threads, so the count must not lose any; the
// thread build also checks that counting does not race.
void threads() {
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < threads_used; ++i) {
        workers.emplace_back([] {
            for (unsigned n = 0; n < checks_per_thread; ++n) CHECK(n < checks_per_thread);
        });
    }
    for (std::thread &worker : workers) worker.join();
    CHECK(amity_test::checks.load() == threads_used * checks_per_thread);
}

void no_checks() {}

bool unused_input = false;
const amity_test::Finisher input_used([] {
    if (unused_input) amity_test::fail(__FILE__, __LINE__, "a finisher found unused input");
});

// A finisher fails a scenario whose own CHECKs passed.
void finisher_failure() {
    CHECK(true);
    unused_input = true;
}

// Had the failed CHECK not ended the process, the scenario would pass with one CHECK.
void failed_check() {
    const int value = 2;
    CHECK(value == 2);
    CHECK(value == 3);
}

}  // namespace

int main(int argc, char **argv) {
    CHECK(argc > 0);  // Counts for no scenario, so no-checks still fails.
    return amity_test::run(argc, argv, {
        {"checks", checks},
        {"threads", threads},
        {"no-checks", no_checks},
        {"failed-check", failed_check},
        {"finisher-failure", finisher_failure},
    });
}
