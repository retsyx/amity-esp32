# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# ESP-IDF's pinned Mbed TLS, built once for the host tests. It uses the upstream
# software crypto configuration, not the ESP32 accelerators. C++ targets link
# host_mbedtls_tls, host_mbedtls_x509 or host_mbedtls_crypto, which carry the
# include directories. Python tests are passed to amity_host_mbedtls_python_tests.
include(ExternalProject)

if(DEFINED ENV{IDF_PATH})
    set(AMITY_HOST_MBEDTLS_SOURCE "$ENV{IDF_PATH}/components/mbedtls/mbedtls")
else()
    set(AMITY_HOST_MBEDTLS_SOURCE "$ENV{HOME}/.espressif/v6.1/esp-idf/components/mbedtls/mbedtls")
endif()
if(NOT EXISTS "${AMITY_HOST_MBEDTLS_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "${AMITY_HOST_MBEDTLS_SOURCE} is missing. Set IDF_PATH, "
        "or configure with -DAMITY_HOST_TESTS=standalone.")
endif()
set(AMITY_HOST_MBEDTLS_BUILD "${CMAKE_BINARY_DIR}/host-mbedtls")

# Espressif's bignum.c and ecp.c include mbedtls/bignum.h and mbedtls/ecp.h,
# which the SDK's port directory supplies. These forward to the upstream
# private headers instead.
set(shims "${AMITY_HOST_MBEDTLS_BUILD}/host-include")
foreach(name IN ITEMS bignum ecp)
    file(CONFIGURE OUTPUT "${shims}/mbedtls/${name}.h" CONTENT "#include \"mbedtls/private/${name}.h\"\n")
endforeach()

set(libraries)
foreach(name IN ITEMS mbedtls mbedx509 tfpsacrypto)
    set(${name} "${AMITY_HOST_MBEDTLS_BUILD}/library/${CMAKE_STATIC_LIBRARY_PREFIX}${name}")
    string(APPEND ${name} "${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND libraries "${${name}}")
endforeach()
ExternalProject_Add(host_mbedtls
    SOURCE_DIR "${AMITY_HOST_MBEDTLS_SOURCE}"
    BINARY_DIR "${AMITY_HOST_MBEDTLS_BUILD}"
    CMAKE_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_FLAGS=-I${shims}"
        -DGEN_FILES=OFF -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
    BUILD_BYPRODUCTS ${libraries}
    INSTALL_COMMAND "")

add_library(host_mbedtls_crypto STATIC IMPORTED GLOBAL)
set_target_properties(host_mbedtls_crypto PROPERTIES IMPORTED_LOCATION "${tfpsacrypto}")
target_include_directories(host_mbedtls_crypto INTERFACE "${AMITY_HOST_MBEDTLS_SOURCE}/tf-psa-crypto/include"
    "${AMITY_HOST_MBEDTLS_SOURCE}/tf-psa-crypto/drivers/builtin/include")
add_dependencies(host_mbedtls_crypto host_mbedtls)
add_library(host_mbedtls_x509 STATIC IMPORTED GLOBAL)
set_target_properties(host_mbedtls_x509 PROPERTIES IMPORTED_LOCATION "${mbedx509}")
target_include_directories(host_mbedtls_x509 INTERFACE "${AMITY_HOST_MBEDTLS_SOURCE}/include")
target_link_libraries(host_mbedtls_x509 INTERFACE host_mbedtls_crypto)
add_library(host_mbedtls_tls STATIC IMPORTED GLOBAL)
set_target_properties(host_mbedtls_tls PROPERTIES IMPORTED_LOCATION "${mbedtls}")
target_link_libraries(host_mbedtls_tls INTERFACE host_mbedtls_x509)

# Python tests find the library through AMITY_HOST_MBEDTLS_SOURCE and
# AMITY_HOST_MBEDTLS (tests/host_mbedtls.py). They require this fixture, which
# builds it for a test run that did not build everything first.
add_test(NAME host_mbedtls_build COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target host_mbedtls)
set_tests_properties(host_mbedtls_build PROPERTIES LABELS sdk TIMEOUT 600 FIXTURES_SETUP host_mbedtls)
function(amity_host_mbedtls_python_tests)
    set_tests_properties(${ARGN} PROPERTIES FIXTURES_REQUIRED host_mbedtls ENVIRONMENT
        "AMITY_HOST_MBEDTLS_SOURCE=${AMITY_HOST_MBEDTLS_SOURCE};AMITY_HOST_MBEDTLS=${AMITY_HOST_MBEDTLS_BUILD}")
endfunction()

unset(shims)
unset(libraries)
unset(mbedtls)
unset(mbedx509)
unset(tfpsacrypto)
