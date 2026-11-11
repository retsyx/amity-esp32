#!/usr/bin/env python3
"""Generate the pinned ESP-IDF AES DMA source with allocation-failure cleanup.

The SDK's second bounce-buffer allocation can fail after its first succeeds.
Route that failure through its existing zeroize/free cleanup. The installed SDK
stays untouched; CMake compiles this generated copy in the same crypto target.
"""
from pathlib import Path
import argparse
import hashlib


# ESP-IDF v6.1 helper, from its declaration through the closing brace, with LF
# line endings and one trailing newline. Review the entire helper on SDK updates.
PINNED_HELPER_SHA256 = 'a4a19f3a5a1526dc91950957f6d0cd12fe534bd51168706ec5ac28e552120a95'


ORIGINAL = '''        if (output_buf == NULL) {
            mbedtls_platform_zeroize(output, len);
            ESP_LOGE(TAG, "Failed to allocate memory");
            return -1;
        }'''
CORRECTED = '''        if (output_buf == NULL) {
            mbedtls_platform_zeroize(output, len);
            ESP_LOGE(TAG, "Failed to allocate memory");
            ret = -1;
            goto cleanup;
        }'''


def patch_source(source: str) -> str:
    signature = 'static int esp_aes_process_dma_ext_ram('
    if source.count(signature) != 1 or source.count(ORIGINAL) != 1:
        raise ValueError('Unrecognized ESP-IDF AES DMA source; review the bounce-buffer cleanup fix')
    start = source.index(signature)
    brace = source.find('{', start)
    if brace < 0:
        raise ValueError('Unrecognized ESP-IDF AES DMA helper declaration')
    depth, end = 1, brace + 1
    while depth and end < len(source):
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    helper = source[start:end].replace('\r\n', '\n') + '\n'
    if depth or hashlib.sha256(helper.encode()).hexdigest() != PINNED_HELPER_SHA256:
        raise ValueError('ESP-IDF AES DMA helper changed; review the complete cleanup implementation')
    cleanup = source.find('\ncleanup:\n', start)
    failure = source.index(ORIGINAL)
    if cleanup < 0 or not start < failure < cleanup:
        raise ValueError('Unrecognized ESP-IDF AES DMA cleanup path')
    return source.replace(ORIGINAL, CORRECTED, 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    corrected = patch_source(args.input.read_bytes().decode())
    if args.input.resolve() == args.output.resolve():
        parser.error('The generated source must not replace the installed SDK source')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != corrected.encode():
        args.output.write_bytes(corrected.encode())


if __name__ == '__main__':
    main()
