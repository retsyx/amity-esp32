# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Host tests: sanitized C/C++ tests built against firmware sources and fakes,
# plus the Python, Node and Go tests, all run by CTest. Use run_host_tests.sh.
cmake_minimum_required(VERSION 3.22)
project(amity_host_tests LANGUAGES C CXX)

# standalone: tests that need no ESP-IDF checkout, managed components or firmware
# build output. sdk: only the tests that need them. all: both.
set(AMITY_HOST_TESTS all CACHE STRING "Host tests to define: all, standalone or sdk")
set_property(CACHE AMITY_HOST_TESTS PROPERTY STRINGS all standalone sdk)
if(NOT AMITY_HOST_TESTS MATCHES "^(all|standalone|sdk)$")
    message(FATAL_ERROR "AMITY_HOST_TESTS must be all, standalone or sdk")
endif()

# address,undefined: the normal build. thread: a separate build for the tests
# labelled race, which run there with ctest -L race.
set(AMITY_HOST_SANITIZER address,undefined CACHE STRING "Host test sanitizers: address,undefined or thread")
set_property(CACHE AMITY_HOST_SANITIZER PROPERTY STRINGS address,undefined thread)
if(NOT AMITY_HOST_SANITIZER MATCHES "^(address,undefined|thread)$")
    message(FATAL_ERROR "AMITY_HOST_SANITIZER must be address,undefined or thread")
endif()

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(T "${ROOT}/tests")
set(C "${ROOT}/components")
set(HAP "${ROOT}/third_party/esp-homekit-sdk/components")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# run_host_tests.sh deletes bin/ before each build, so a test whose build
# fails cannot run a stale executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_program(PYTHON3 python3 REQUIRED)
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

enable_testing()

add_library(sanitized INTERFACE)
target_compile_options(sanitized INTERFACE -g -fsanitize=${AMITY_HOST_SANITIZER})
target_link_options(sanitized INTERFACE -fsanitize=${AMITY_HOST_SANITIZER})
add_library(strict INTERFACE)
target_compile_options(strict INTERFACE -Wall -Wextra -Werror)
target_link_libraries(strict INTERFACE sanitized)

# Firmware sources shared by many tests, compiled once.
add_library(amity_model STATIC "${C}/amity_system/model.cpp")
target_include_directories(amity_model PRIVATE "${C}/amity_system/include")
target_link_libraries(amity_model PRIVATE strict)

# The host-compilable consortium codecs. The other consortium sources are
# ESP_PLATFORM-only; a static library links only the objects a test uses.
add_library(amity_consortium STATIC
    "${C}/consortium/bootstrap.cpp" "${C}/consortium/enrollment.cpp" "${C}/consortium/json.cpp"
    "${C}/consortium/owned_intent.cpp" "${C}/consortium/radio.cpp" "${C}/consortium/session.cpp"
    "${C}/consortium/settings.cpp" "${C}/consortium/update.cpp" "${C}/consortium/wire.cpp")
target_include_directories(amity_consortium PRIVATE "${C}/consortium/include" "${C}/amity_system/include")
target_link_libraries(amity_consortium PRIVATE strict)

# Fakes and seams define functions that SDK or firmware headers declare. Each
# definition must match an earlier declaration, so a changed signature fails
# to compile instead of adding an unused overload.
set(amity_seam_warnings $<$<CXX_COMPILER_ID:GNU>:-Werror=missing-declarations>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Werror=missing-prototypes>)

# The core SDK fakes: error codes, logs, timers, heap capabilities, image
# descriptors and FreeRTOS on a scripted clock. A test links core_fakes and
# lists "${T}/core_fakes" first among its INCLUDES, ahead of tests/fakes.
add_library(core_fakes OBJECT "${T}/core_fakes/core_fakes.cpp")
target_include_directories(core_fakes PRIVATE "${T}/core_fakes" "${T}")
target_compile_options(core_fakes PRIVATE ${amity_seam_warnings})
target_link_libraries(core_fakes PRIVATE strict)

# PSA Crypto over OpenSSL, with the SDK's types and constants, and opaque but
# complete Mbed TLS types. Tests list "${T}/crypto_fakes" among their INCLUDES.
add_library(crypto_fakes OBJECT "${T}/crypto_fakes/crypto_fakes.cpp")
target_include_directories(crypto_fakes PRIVATE "${T}/crypto_fakes" "${T}")
target_compile_options(crypto_fakes PRIVATE ${amity_seam_warnings})
target_link_libraries(crypto_fakes PRIVATE strict PUBLIC OpenSSL::Crypto)

# AMITY_TEST_TIER is set only while the area files are read. The functions
# below label each test with it, and fail anywhere else.
function(amity_require_tier name)
    if(NOT AMITY_TEST_TIER)
        message(FATAL_ERROR "Register ${name} in a tests/cmake/<area>.cmake file")
    endif()
endfunction()

# amity_test(<name> SOURCES ... [INCLUDES ...] [LIBS ...] [DEFINES ...] [PTHREAD] [RACE]
#            [ARGS ...] [SCENARIOS ... [REGISTRY]] [FIXTURES ...])
# Each scenario becomes its own test, <name>.<scenario>. REGISTRY, for tests
# built on tests/scenario_main.hpp, adds <name>.scenarios, which fails unless
# the scenario table compiled into the test names exactly the SCENARIOS. RACE
# labels the tests race, so the thread build runs them too.
function(amity_test name)
    cmake_parse_arguments(PARSE_ARGV 1 A "PTHREAD;REGISTRY;RACE" "" "SOURCES;INCLUDES;LIBS;DEFINES;ARGS;SCENARIOS;FIXTURES")
    amity_require_tier(${name})
    if(A_REGISTRY AND NOT A_SCENARIOS)
        message(FATAL_ERROR "amity_test(${name}): REGISTRY needs SCENARIOS")
    endif()
    add_executable(${name} ${A_SOURCES})
    # tests/fakes holds the fake headers shared by several tests; each test's
    # own fake directories come first.
    target_include_directories(${name} PRIVATE ${A_INCLUDES} "${T}/fakes")
    target_compile_definitions(${name} PRIVATE ${A_DEFINES})
    target_link_libraries(${name} PRIVATE ${A_LIBS} strict)
    if(A_PTHREAD)
        target_compile_options(${name} PRIVATE -pthread)
        target_link_options(${name} PRIVATE -pthread)
    endif()
    set(tests)
    if(A_SCENARIOS)
        foreach(scenario IN LISTS A_SCENARIOS)
            add_test(NAME ${name}.${scenario} COMMAND ${name} ${scenario} WORKING_DIRECTORY "${ROOT}")
            list(APPEND tests ${name}.${scenario})
        endforeach()
        if(A_REGISTRY)
            add_test(NAME ${name}.scenarios COMMAND ${name} --verify-scenarios ${A_SCENARIOS}
                WORKING_DIRECTORY "${ROOT}")
            list(APPEND tests ${name}.scenarios)
        endif()
    else()
        add_test(NAME ${name} COMMAND ${name} ${A_ARGS} WORKING_DIRECTORY "${ROOT}")
        set(tests ${name})
    endif()
    set(labels ${AMITY_TEST_TIER})
    if(A_RACE)
        list(APPEND labels race)
    endif()
    set_tests_properties(${tests} PROPERTIES LABELS "${labels}" TIMEOUT 600)
    if(A_FIXTURES)
        set_tests_properties(${tests} PROPERTIES FIXTURES_REQUIRED "${A_FIXTURES}")
    endif()
endfunction()

# amity_host_compile(<name> SOURCES ... [INCLUDES ...] [DEFINES ...])
# Compiles firmware sources with the strict flags and links nothing, so every
# build checks that they still compile on the host, and they appear in
# compile_commands.json.
function(amity_host_compile name)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "SOURCES;INCLUDES;DEFINES")
    amity_require_tier(${name})
    add_library(${name} OBJECT ${A_SOURCES})
    target_include_directories(${name} PRIVATE ${A_INCLUDES} "${T}/fakes")
    target_compile_definitions(${name} PRIVATE ${A_DEFINES})
    target_link_libraries(${name} PRIVATE strict)
endfunction()

function(amity_command_test name)
    amity_require_tier(${name})
    add_test(NAME ${name} COMMAND ${ARGN} WORKING_DIRECTORY "${ROOT}")
    set_tests_properties(${name} PROPERTIES LABELS ${AMITY_TEST_TIER} TIMEOUT 600)
endfunction()

# Script tests are named after their file. Standalone scripts run without
# site-packages, like CI's bare interpreter, so they use only the standard library.
function(amity_python_tests)
    set(site)
    if(AMITY_TEST_TIER STREQUAL standalone)
        set(site -S)
    endif()
    foreach(script IN LISTS ARGN)
        amity_command_test(${script} "${PYTHON3}" ${site} "${T}/${script}")
    endforeach()
endfunction()

function(amity_node_tests)
    foreach(script IN LISTS ARGN)
        amity_command_test(${script} "${NODE}" "${T}/${script}")
    endforeach()
endfunction()

# Tests that need no ESP-IDF checkout, managed components or firmware build.
if(AMITY_HOST_TESTS MATCHES "^(all|standalone)$")
    find_program(NODE node REQUIRED)
    find_program(GO go REQUIRED)
endif()

# Tests that read ESP-IDF sources, managed components or firmware build output,
# or need the SDK's Python packages.
if(AMITY_HOST_TESTS MATCHES "^(all|sdk)$")
    set(json "${ROOT}/managed_components/espressif__cjson/cJSON")
    if(NOT EXISTS "${json}/cJSON.c")
        set(json "${ROOT}/managed_components/espressif__cjson")
    endif()
    if(NOT EXISTS "${json}/cJSON.c")
        message(FATAL_ERROR "managed_components/espressif__cjson is missing. Build the firmware once, "
            "or configure with -DAMITY_HOST_TESTS=standalone.")
    endif()
    add_library(cjson STATIC "${json}/cJSON.c")
    target_include_directories(cjson PRIVATE "${json}")
    target_compile_options(cjson PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(cjson PRIVATE sanitized)
endif()

# Each area registers its tests in its own tests/cmake/<area>.cmake, so work on
# one area never edits another's file. Each file is read once for every tier
# being defined, with AMITY_TEST_TIER naming that tier, and registers only that
# tier's tests. Register one Python or Node test per line.
foreach(AMITY_TEST_TIER IN ITEMS standalone sdk)
    if(AMITY_HOST_TESTS MATCHES "^(all|${AMITY_TEST_TIER})$")
        foreach(area IN ITEMS consortium_runtime consortium_gatt consortium_tls web_system_admin web_admin
                web_server system_services matter homekit main sdk_slices other)
            include("${T}/cmake/${area}.cmake")
        endforeach()
    endif()
endforeach()
