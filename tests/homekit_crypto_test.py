#!/usr/bin/env python3
"""Check HomeKit's PSA-backed HKDF-SHA-512 and SHA-512 against independent references."""
import hashlib
import hmac
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_tls_credentials_test import native_library

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / 'third_party/esp-homekit-sdk/components/hkdf-sha'


def reference_hkdf(salt, ikm, info, length):
    key = hmac.new(salt, ikm, hashlib.sha512).digest()
    output, block = b'', b''
    for counter in range(1, -(-length // 64) + 1):
        block = hmac.new(key, block + info + bytes([counter]), hashlib.sha512).digest()
        output += block
    return output[:length]


def c_bytes(data):
    return '{' + ','.join(str(byte) for byte in data) + '}' if data else '{0}'


def main():
    cases = [(b'Pair-Setup-Encrypt-Salt', bytes(range(64)), b'Pair-Setup-Encrypt-Info', 32),
             (b'Control-Salt', bytes(range(255, 223, -1)), b'Control-Read-Encryption-Key', 32),
             (b'Pair-Setup-Controller-Sign-Salt', b'\x00' * 384, b'Pair-Setup-Controller-Sign-Info', 150)]
    messages = [b'', b'abc', bytes(range(256)) * 5]
    lines = ['#include "hkdf-sha.h"', '#include <assert.h>', '#include <stdio.h>', '#include <string.h>', 'int main(void) {']
    for index, (salt, ikm, info, length) in enumerate(cases):
        lines.append(f'{{static const unsigned char salt[]={c_bytes(salt)},ikm[]={c_bytes(ikm)},info[]={c_bytes(info)};'
                     f'static const uint8_t expected[]={c_bytes(reference_hkdf(salt, ikm, info, length))};uint8_t okm[{length}];'
                     f'assert(hkdf(SHA512,salt,{len(salt)},ikm,{len(ikm)},info,{len(info)},okm,{length})==shaSuccess);'
                     f'assert(!memcmp(okm,expected,{length}));}}')
    lines.append('SHA512Context context;uint8_t digest[SHA512HashSize];')
    for message in messages:
        half = len(message) // 2
        lines.append(f'{{static const uint8_t message[]={c_bytes(message)},expected[]={c_bytes(hashlib.sha512(message).digest())};'
                     f'assert(SHA512Reset(&context)==shaSuccess);'
                     f'assert(SHA512Input(&context,message,{half})==shaSuccess);'
                     f'assert(SHA512Input(&context,message+{half},{len(message) - half})==shaSuccess);'
                     f'assert(SHA512Result(&context,digest)==shaSuccess&&!memcmp(digest,expected,64));}}')
    lines.append('puts("PASS HomeKit HKDF-SHA-512 and SHA-512 use PSA and match independent reference values");return 0;}')
    source, build = native_library()
    with tempfile.TemporaryDirectory(prefix='amity-homekit-crypto-') as directory:
        test, binary = Path(directory) / 'test.c', Path(directory) / 'test'
        test.write_text('\n'.join(lines) + '\n')
        include = [COMPONENT / 'include', source / 'include', source / 'tf-psa-crypto/include',
                   source / 'tf-psa-crypto/drivers/builtin/include', build / 'tf-psa-crypto/include']
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            *['-I' + str(path) for path in include], str(test), str(COMPONENT / 'hkdf_sha.c'),
            str(build / 'library/libtfpsacrypto.a'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
