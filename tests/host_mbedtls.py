"""Build the pinned ESP-IDF Mbed TLS as a native library for the host tests that link it.

It uses the upstream software crypto configuration, not the ESP32 accelerators.
"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))


def native_library():
    source=IDF/'components/mbedtls/mbedtls'
    build=ROOT/'test-results/host/consortium-mbedtls'
    shim=build/'host-include/mbedtls';shim.mkdir(parents=True,exist_ok=True)
    # Espressif source includes public compatibility wrappers during the native
    # software build. The wrappers only expose the same upstream private types.
    for name in ('bignum','ecp'):
        header=shim/(name+'.h');content='#include "mbedtls/private/'+name+'.h"\n'
        if not header.exists() or header.read_text()!=content:header.write_text(content)
    configure=['cmake','-S',str(source),'-B',str(build),'-DGEN_FILES=OFF','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF','-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-I'+str(shim.parent)]
    for command in (configure,['cmake','--build',str(build),'-j','8']):
        result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if result.returncode:
            print(result.stdout);result.check_returncode()
    return source,build
