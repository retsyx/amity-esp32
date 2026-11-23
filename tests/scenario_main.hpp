/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario tests run one scenario per process, so no scenario needs to reset
// state another one left behind. main() passes the test's scenario table to
// amity_test::run():
//
//   int main(int argc, char **argv) {
//       return amity_test::run(argc, argv, {{"idle", idle}, {"hold", hold}});
//   }
//
// amity_test(<name> REGISTRY SCENARIOS idle hold) runs `<name> idle` and
// `<name> hold`, plus `<name> --verify-scenarios idle hold`, which fails unless
// the CMake list and the table name the same scenarios. `<name> --list` prints
// the table. A scenario fails when it is unknown, when a CHECK fails, and when
// it ran no CHECK at all, so a scenario that dispatches nowhere cannot pass.
// After a scenario returns, each registered Finisher runs; a fake registers one
// to fail a scenario that left scripted input unused.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace amity_test {

struct Scenario {
    const char *name;
    void (*run)();
};

// CHECKs passed by the running scenario, counted from any thread.
inline std::atomic<unsigned long> checks{0};

inline void checked() { checks.fetch_add(1, std::memory_order_relaxed); }

// Checks that run, in the order they were registered, after each scenario
// that returns. A check ends the scenario with fail(); it does not CHECK, so it
// cannot make a scenario that ran no CHECK pass.
using Finish = void (*)();
inline std::vector<Finish> &finishers() {
    static std::vector<Finish> list;
    return list;
}
// Registers a check from a namespace-scope object.
struct Finisher {
    explicit Finisher(Finish check) { finishers().push_back(check); }
};

// Reports a failure and ends the process at once, from any thread: destructors
// and exit handlers would run on state the failure left inconsistent.
[[noreturn]] inline void fail(const char *file, int line, const char *message) {
    std::fprintf(stderr, "%s:%d: %s\n", file, line, message);
    std::fflush(stderr);
    std::_Exit(1);
}

inline const Scenario *find(std::initializer_list<Scenario> scenarios, const char *name) {
    for (const Scenario &scenario : scenarios) {
        if (std::strcmp(scenario.name, name) == 0) return &scenario;
    }
    return nullptr;
}

// Compares the scenarios registered with CTest with the table in both directions.
inline int verify(std::initializer_list<Scenario> scenarios, int count, char **names) {
    int status = 0;
    for (int i = 0; i < count; ++i) {
        bool repeated = false;
        for (int j = 0; j < i; ++j) repeated = repeated || std::strcmp(names[i], names[j]) == 0;
        if (repeated) {
            std::fprintf(stderr, "scenario '%s' is registered twice\n", names[i]);
            status = 1;
        } else if (!find(scenarios, names[i])) {
            std::fprintf(stderr, "scenario '%s' is registered but not in the test\n", names[i]);
            status = 1;
        }
    }
    for (const Scenario &scenario : scenarios) {
        bool registered = false;
        for (int i = 0; i < count; ++i) registered = registered || std::strcmp(scenario.name, names[i]) == 0;
        if (!registered) {
            std::fprintf(stderr, "scenario '%s' is in the test but not registered\n", scenario.name);
            status = 1;
        }
    }
    return status;
}

inline int run(int argc, char **argv, std::initializer_list<Scenario> scenarios) {
    for (const Scenario &scenario : scenarios) {
        if (find(scenarios, scenario.name) != &scenario) {
            std::fprintf(stderr, "scenario '%s' is in the table twice\n", scenario.name);
            return 2;
        }
    }
    if (argc >= 2 && std::strcmp(argv[1], "--verify-scenarios") == 0) return verify(scenarios, argc - 2, argv + 2);
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Scenario &scenario : scenarios) std::printf("%s\n", scenario.name);
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scenario> | --list | --verify-scenarios <scenario>...\n", argv[0]);
        return 2;
    }
    const Scenario *scenario = find(scenarios, argv[1]);
    if (!scenario) {
        std::fprintf(stderr, "unknown scenario '%s'\n", argv[1]);
        return 2;
    }
    checks.store(0);
    scenario->run();
    if (checks.load() == 0) {
        std::fprintf(stderr, "scenario '%s' ran no CHECK\n", scenario->name);
        return 1;
    }
    for (const Finish finish : finishers()) finish();
    return 0;
}

}  // namespace amity_test

// Counts one check, or reports the failed expression and ends the process.
#define CHECK(expression) \
    ((expression) ? amity_test::checked() : amity_test::fail(__FILE__, __LINE__, "CHECK(" #expression ") failed"))
