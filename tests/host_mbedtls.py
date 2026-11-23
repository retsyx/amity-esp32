"""Find the pinned ESP-IDF Mbed TLS that the host test build compiles for the tests that link it.

tests/cmake/host_mbedtls.cmake builds it with the upstream software crypto
configuration, not the ESP32 accelerators, and gives its source and build
directories to these tests in AMITY_HOST_MBEDTLS_SOURCE and AMITY_HOST_MBEDTLS.
"""
import os
from pathlib import Path


def native_library():
    """Return the Mbed TLS source and build directories."""
    if not os.environ.get('AMITY_HOST_MBEDTLS'):
        raise SystemExit('AMITY_HOST_MBEDTLS is not set: run this test through CTest')
    return Path(os.environ['AMITY_HOST_MBEDTLS_SOURCE']), Path(os.environ['AMITY_HOST_MBEDTLS'])
