#!/usr/bin/env python3
"""Exercise the pinned/generated AES helper through allocation and DMA failures."""
from pathlib import Path
import importlib.util
import os
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))
GENERATOR = ROOT / 'tools/patch_esp_aes_dma.py'
spec = importlib.util.spec_from_file_location('patch_esp_aes_dma', GENERATOR)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

PREFIX = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 0
#define AES_MAX_CHUNK_WRITE_SIZE 1600
#define MALLOC_CAP_DMA 1
#define MIN(a,b) std::min(size_t(a),size_t(b))
#define ESP_LOGE(...) do {} while(0)
struct Block {void* address;size_t bytes;};
std::vector<Block> allocations;
size_t live=0,allocation_calls=0,fail_allocation=0,crypto_calls=0,fail_crypto=0,frees=0;
unsigned char* heap_caps_aligned_alloc(size_t,size_t size,unsigned caps){
    assert(caps==MALLOC_CAP_DMA);
    if(++allocation_calls==fail_allocation)return nullptr;
    auto* result=static_cast<unsigned char*>(std::malloc(size));assert(result);
    std::memset(result,0xa5,size);allocations.push_back({result,size});live+=size;return result;
}
void tracked_free(void* memory){
    if(!memory)return;
    auto block=std::find_if(allocations.begin(),allocations.end(),[&](const auto& b){return b.address==memory;});
    assert(block!=allocations.end());
    // Existing SDK cleanup must zero every byte of each temporary allocation.
    const auto* bytes=static_cast<const unsigned char*>(memory);
    assert(std::all_of(bytes,bytes+block->bytes,[](auto byte){return byte==0;}));
    live-=block->bytes;allocations.erase(block);++frees;std::free(memory);
}
void mbedtls_platform_zeroize(void* memory,size_t bytes){std::memset(memory,0,bytes);}
struct esp_aes_context{};
int esp_aes_process_dma(esp_aes_context*,const unsigned char* input,unsigned char* output,size_t size,uint8_t*){
    if(++crypto_calls==fail_crypto)return -1;
    for(size_t i=0;i<size;++i)output[i]=input[i]^0x5a;
    return 0;
}
#define free tracked_free
'''

SUITE = r'''
#undef free
using Helper=int(*)(esp_aes_context*,const unsigned char*,unsigned char*,size_t,uint8_t*,bool,bool);
void reset(size_t fail=0,size_t dma_fail=0){
    assert(live==0&&allocations.empty());allocation_calls=crypto_calls=frees=0;
    fail_allocation=fail;fail_crypto=dma_fail;
}
int invoke(Helper helper,size_t size,bool input_copy,bool output_copy,bool output_must_be_zero=false){
    std::vector<unsigned char> input(size),output(size,0xa5);
    for(size_t i=0;i<size;++i)input[i]=static_cast<unsigned char>(i);
    esp_aes_context context;
    const auto result=helper(&context,input.data(),output.data(),size,nullptr,input_copy,output_copy);
    if(output_must_be_zero)assert(std::all_of(output.begin(),output.end(),[](auto byte){return byte==0;}));
    if(result==0)for(size_t i=0;i<size;++i)assert(output[i]==static_cast<unsigned char>(input[i]^0x5a));
    return result;
}
int main(){
    // The unchanged installed SDK reproduces the leak before testing the fix.
    reset(2);assert(invoke(sdk_original,512,true,true,true)==-1);
    assert(allocation_calls==2&&crypto_calls==0&&frees==0&&live==512&&allocations.size()==1);
    // Release only the fixture's observed leaked pointer so sanitizers can
    // audit the remaining cases without intentionally leaking the process.
    mbedtls_platform_zeroize(allocations[0].address,allocations[0].bytes);tracked_free(allocations[0].address);
    reset(2);assert(invoke(fixed,512,true,true,true)==-1);
    assert(allocation_calls==2&&crypto_calls==0&&frees==1&&live==0);
    for(const auto copies:{std::array<bool,2>{true,true},{true,false},{false,true}}){
        reset(1);assert(invoke(fixed,512,copies[0],copies[1],true)==-1);
        assert(allocation_calls==1&&crypto_calls==0&&frees==0&&live==0);
    }
    for(const auto size:{size_t{16},size_t{512},size_t{1600},size_t{4096}})
        for(const auto copies:{std::array<bool,2>{true,true},{true,false},{false,true},{false,false}}){
            reset();assert(invoke(fixed,size,copies[0],copies[1])==0);
            assert(allocation_calls==size_t(copies[0])+size_t(copies[1])&&frees==allocation_calls&&live==0);
            assert(crypto_calls==(size+1599)/1600);
        }
    for(const auto failed_chunk:{size_t{1},size_t{2}}){
        reset(0,failed_chunk);assert(invoke(fixed,4096,true,true)==-1);
        assert(crypto_calls==failed_chunk&&allocation_calls==2&&frees==2&&live==0);
    }
    std::puts("PASS actual pinned AES: original second-allocation leak reproduced; generated cleanup zeroizes/frees all temporary buffers on allocation and DMA errors; success/chunk behavior preserved");
}
'''


def main() -> None:
    sdk = IDF / 'components/mbedtls/port/aes/dma/esp_aes_dma_core.c'
    original = sdk.read_text()
    expected = patch.patch_source(original)
    assert expected == original.replace(patch.ORIGINAL, patch.CORRECTED, 1)
    for unknown in ('', expected, original + patch.ORIGINAL,
                    original.replace('if (output_buf == NULL)', 'if (!output_buf)'),
                    original.replace('free(input_buf);', 'free(NULL);'),
                    original.replace('mbedtls_platform_zeroize(input_buf, alloc_chunk_len);', ''),
                    original.replace('ret = -1;\n            goto cleanup;', 'return -1;')):
        try:
            patch.patch_source(unknown)
        except ValueError:
            pass
        else:
            raise AssertionError('Generator accepted an unknown SDK cleanup preimage')
    # Unrelated SDK content stays verbatim; only the complete pinned helper is
    # the patch preimage, rather than the whole SDK translation unit.
    decorated = '// unrelated SDK prefix\n' + original + '\n// unrelated SDK suffix\n'
    assert patch.patch_source(decorated) == '// unrelated SDK prefix\n' + expected + '\n// unrelated SDK suffix\n'
    with tempfile.TemporaryDirectory(prefix='amity-aes-cleanup-') as directory:
        temp = Path(directory)
        generated, cpp, binary = temp / 'generated.c', temp / 'test.cpp', temp / 'test'
        subprocess.run(['python3', str(GENERATOR), '--input', str(sdk), '--output', str(generated)], check=True)
        assert generated.read_text() == expected and sdk.read_text() == original
        helper = 'static int esp_aes_process_dma_ext_ram('
        original_helper = function(original, helper).replace(helper, 'static int sdk_original(', 1)
        fixed_helper = function(generated.read_text(), helper).replace(helper, 'static int fixed(', 1)
        cpp.write_text(PREFIX + original_helper + fixed_helper + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
