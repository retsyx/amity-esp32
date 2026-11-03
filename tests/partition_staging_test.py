#!/usr/bin/env python3
"""Execute production partition storage/staging with an in-memory flash device."""
import os
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import partition_image as partition_tool
import hardware_profile

PREFIX=r'''
#include "firmware_update.hpp"
#include "partition_image.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <openssl/sha.h>
using esp_ota_handle_t=int;
constexpr unsigned CONFIG_IDF_FIRMWARE_CHIP_ID=9,ESP_IMAGE_HEADER_MAGIC=0xe9;
constexpr uint32_t ESP_APP_DESC_MAGIC_WORD=0xabcd5432;
struct esp_image_header_t{uint8_t magic=ESP_IMAGE_HEADER_MAGIC,bytes[11]{};uint16_t chip_id=9;uint8_t end[10]{};};
struct esp_image_segment_header_t{uint32_t address=0,length=0;};
struct esp_app_desc_t{uint32_t magic_word=ESP_APP_DESC_MAGIC_WORD,secure=0,reserved[2]{};char version[32]="0.1.133",project_name[32]="amity",time[16]{},date[16]{},sdk[32]{};uint8_t app_elf_sha256[32]{};uint8_t tail[80]{};};
static_assert(sizeof(esp_image_header_t)==24&&sizeof(esp_app_desc_t)==256);
esp_partition_t current{0x3d0000,0},candidate{0x3d0000,1};
std::array<std::string,2> memory{std::string(0x3d0000,'\xff'),std::string(0x3d0000,'\xff')};
const esp_partition_t* selected=&current;
esp_app_desc_t app;
int64_t clock_us=0;int reboots=0,maintenance=0,failures=0,ends=0,activations=0,report_staged=0;
unsigned flash_stack_depth=0,flash_dispatches=0;
bool native_valid=true;int fail_write=-1,writes=0;
uint32_t reboot_delay=0;
int64_t esp_timer_get_time(){return clock_us;}
const esp_app_desc_t*esp_app_get_description(){return &app;}
const esp_partition_t*esp_ota_get_running_partition(){return &current;}
const esp_partition_t*esp_ota_get_next_update_partition(void*){return &candidate;}
int esp_partition_read(const esp_partition_t*p,size_t off,void*out,size_t count){assert(flash_stack_depth==1&&off+count<=p->size);memcpy(out,memory[p->index].data()+off,count);return ESP_OK;}
int esp_partition_write(const esp_partition_t*p,size_t off,const void*input,size_t count){assert(flash_stack_depth==1&&off%16==0&&count%16==0&&off+count<=p->size);if(writes++==fail_write)return ESP_FAIL;const auto*bytes=static_cast<const char*>(input);for(size_t i=0;i<count;++i){assert((static_cast<unsigned char>(memory[p->index][off+i])&static_cast<unsigned char>(bytes[i]))==static_cast<unsigned char>(bytes[i]));memory[p->index][off+i]=bytes[i];}return ESP_OK;}
int esp_partition_erase_range(const esp_partition_t*p,size_t off,size_t count){assert(flash_stack_depth==1&&off%4096==0&&count%4096==0&&off+count<=p->size);memory[p->index].replace(off,count,count,'\xff');return ESP_OK;}
int esp_ota_get_partition_description(const esp_partition_t*p,esp_app_desc_t*out){assert(flash_stack_depth==1);const auto result=esp_partition_read(p,32,out,sizeof(*out));return result==ESP_OK&&out->magic_word==ESP_APP_DESC_MAGIC_WORD?ESP_OK:ESP_FAIL;}
size_t ota_offset=0;int esp_ota_begin(const esp_partition_t*p,size_t count,esp_ota_handle_t*out){assert(flash_stack_depth==1&&p==&candidate&&count<=p->size);ota_offset=0;memory[1].replace(0,count,count,'\xff');*out=1;return ESP_OK;}
int esp_ota_write(int handle,const void*input,size_t count){assert(flash_stack_depth==1&&handle==1);memcpy(memory[1].data()+ota_offset,input,count);ota_offset+=count;return ESP_OK;}
int esp_ota_abort(int){assert(flash_stack_depth==1);return ESP_OK;}
int esp_ota_end(int){assert(flash_stack_depth==1);++ends;return native_valid?ESP_OK:ESP_FAIL;}
int esp_ota_set_boot_partition(const esp_partition_t*p){assert(flash_stack_depth==1);selected=p;++activations;return ESP_OK;}
void reboot_later(uint32_t delay){++reboots;reboot_delay=delay;}
std::string sha(std::string_view bytes){uint8_t digest[32];SHA256(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),digest);std::string out;const char*digits="0123456789abcdef";for(auto b:digest){out+=digits[b>>4];out+=digits[b&15];}return out;}
struct psa_hash_operation_t{std::string bytes;};
#define PSA_HASH_OPERATION_INIT {}
constexpr int PSA_SUCCESS=0,PSA_ALG_SHA_256=1;
int psa_hash_setup(psa_hash_operation_t*op,int){op->bytes.clear();return 0;}
int psa_hash_update(psa_hash_operation_t*op,const uint8_t*bytes,size_t count){op->bytes.append(reinterpret_cast<const char*>(bytes),count);return 0;}
int psa_hash_finish(psa_hash_operation_t*op,uint8_t*out,size_t,size_t*count){SHA256(reinterpret_cast<const uint8_t*>(op->bytes.data()),op->bytes.size(),out);*count=32;return 0;}
int psa_hash_abort(psa_hash_operation_t*op){op->bytes.clear();return 0;}
namespace config_store{std::string sha256_hex(std::string_view bytes){return sha(bytes);}bool with_internal_flash_stack(const std::function<void()>&call){if(flash_stack_depth){assert(flash_stack_depth==1);call();return true;}++flash_stack_depth;++flash_dispatches;call();--flash_stack_depth;return true;}}
namespace system_services::updates{
ReleaseManifest expected;
std::string proof(128,'a');
std::optional<ReleaseManifest> admission;
int validate_release(std::string_view manifest,std::string_view signature,ReleaseManifest&out,std::string&error){if(manifest!=expected.canonical||signature!=proof){error="Invalid publisher release";return ESP_ERR_INVALID_ARG;}out=expected;return ESP_OK;}
bool parse_release_manifest(std::string_view manifest,ReleaseManifest&out,std::string&error){return validate_release(manifest,proof,out,error)==ESP_OK;}
int stage_release(std::string_view manifest,std::string_view signature,std::string&error){ReleaseManifest value;const int result=validate_release(manifest,signature,value,error);if(result==ESP_OK)admission=value;return result;}
std::optional<ReleaseManifest> staged_release(){return admission;}
int begin_install(std::string&){++maintenance;return ESP_OK;}
int install_checkpoint(uint64_t){return ESP_OK;}
void install_failed(const std::string&){++failures;if(maintenance)--maintenance;}
int install_staged(){++report_staged;--maintenance;return ESP_OK;}
int install_selected(){return ESP_OK;}
}
'''

SUITE=r'''
int main(){using namespace system_services::updates;
 for(unsigned i=0;i<32;++i)app.app_elf_sha256[i]=i+1;
 std::string native(5000,'x');esp_image_header_t image;esp_image_segment_header_t segment;
 memcpy(native.data(),&image,sizeof(image));memcpy(native.data()+24,&segment,sizeof(segment));memcpy(native.data()+32,&app,sizeof(app));
 expected.canonical="canonical-publisher-release";expected.release_id="release-one";expected.image_bytes=native.size();expected.image_sha256=sha(native);expected.firmware_version=app.version;
 char identity[65]{};for(unsigned i=0;i<32;++i)snprintf(identity+2*i,3,"%02x",app.app_elf_sha256[i]);expected.image_elf_sha256=identity;
 std::string error,signature,bytes;ReleaseManifest stored;
 auto upload=[&](bool corrupt=false){assert(peer_begin(expected.canonical,proof,error)==ESP_OK);for(size_t off=0;off<native.size();off+=2048){auto block=native.substr(off,std::min<size_t>(2048,native.size()-off));if(corrupt&&off>0)block[0]^=1;assert(peer_chunk(expected.release_id,off,block,error)==ESP_OK);}};
 assert(partition_release(false,stored,signature,error)!=ESP_OK);
 upload();assert(maintenance==1&&selected==&current&&!reboots);
 assert(peer_stage(expected.release_id,error)==ESP_OK&&ends==1&&maintenance==0&&report_staged==1&&selected==&current&&!reboots);
 const auto verification_dispatches=flash_dispatches;
 assert(partition_release(false,stored,signature,error)==ESP_OK&&stored.canonical==expected.canonical&&signature==proof);
 assert(flash_dispatches==verification_dispatches+1); // One task handoff covers metadata, descriptor and every hash block.
 // A lost TLS connection and idle timeout cannot abort a completed image.
 peer_abort("connection closed");clock_us+=3600000000;poll_peer_timeout();admission.reset();
 assert(partition_release(false,stored,signature,error)==ESP_OK&&selected==&current&&!reboots);
 assert(peer_stage(expected.release_id,error)==ESP_OK);
 assert(peer_install("wrong-release",error)!=ESP_OK&&!reboots&&selected==&current);
 const auto chunk_dispatches=flash_dispatches;
 assert(read_staged_image(2048,2048,bytes,error)==ESP_OK&&bytes==native.substr(2048,2048));
 assert(flash_dispatches==chunk_dispatches+1); // Header and image bytes share the same internal-stack dispatch.
 assert(read_staged_image(native.size(),1,bytes,error)!=ESP_OK);
 assert(peer_install(expected.release_id,error,4000)==ESP_OK&&selected==&candidate&&reboots==1&&reboot_delay==4000);
 // The same self-contained bytes are a source after becoming the running slot.
 memory[0]=memory[1];assert(partition_release(true,stored,signature,error)==ESP_OK);
 assert(read_running_image(0,2048,bytes,error)==ESP_OK&&bytes==native.substr(0,2048));
 maintenance=0;selected=&current;reboots=0;
 upload(true);assert(peer_stage(expected.release_id,error)!=ESP_OK&&!reboots&&selected==&current);
 assert(partition_release(false,stored,signature,error)!=ESP_OK); // A new stream invalidated the previous trailer.
 upload();native_valid=false;assert(peer_stage(expected.release_id,error)!=ESP_OK&&selected==&current);native_valid=true;
 upload();fail_write=writes;assert(peer_stage(expected.release_id,error)!=ESP_OK);fail_write=-1;
 assert(partition_release(false,stored,signature,error)!=ESP_OK); // Payload-only/partial trailer is never staged.
 upload();assert(peer_stage(expected.release_id,error)==ESP_OK);
 memory[1][1000]^=1;assert(partition_release(false,stored,signature,error)!=ESP_OK);
 memory[1][1000]^=1;memory[1][candidate.size-partition_release_bytes+partition_release_header_bytes]^=1;
 assert(partition_release(false,stored,signature,error)!=ESP_OK);
 upload();clock_us+=11000000;poll_peer_timeout();assert(maintenance==1);clock_us+=110000000;poll_peer_timeout();assert(partition_release(false,stored,signature,error)!=ESP_OK&&maintenance==0);
 assert(flash_stack_depth==0&&flash_dispatches>0);
 std::puts("PASS durable partition staging: all flash reads/descriptors/writes on internal stack, full image/native validation, no early activation, TLS loss/reboot-safe metadata, running-image source, partial/corrupt trailer and timeout rejection");
}
'''


class PartitionStagingTest(unittest.TestCase):
    def test_provisioned_partition_contains_exact_release(self):
        image=bytearray(288);image[0]=0xe9;image[32:36]=bytes.fromhex('3254cdab');image[176:208]=bytes(range(32))
        image+=hardware_profile.descriptor({'AMITY_BOARD_LEGACY_1T':'y','AMITY_BOARD_DEVELOPMENT_4T':'n'})
        release={'image_bytes':str(len(image)),'image_sha256':hashlib.sha256(image).hexdigest(),'image_elf_sha256':image[176:208].hex(),'security_profile':'development','signing_key_id':'','hardware_profiles':['esp32s3-legacy-1t-v1']}
        for signed in (False,True):
            signature=b'x'*64 if signed else b''
            release['security_profile']='production' if signed else 'development'
            release['signing_key_id']='a'*64 if signed else ''
            manifest=json.dumps(release,sort_keys=True,separators=(',',':')).encode()
            storage=partition_tool.partition_image(bytes(image),manifest,signature)
            self.assertEqual(partition_tool.unpack_partition(storage),(bytes(image),manifest,signature))
            self.assertEqual(len(storage),0x3d0000)
            self.assertEqual(storage[-0x3000:-0x3000+16],b'AMITY-IMAGE-V1\n\0')
            for index in (0,-0x3000,-0x3000+64):
                corrupt=bytearray(storage);corrupt[index]^=1
                with self.assertRaises(ValueError):partition_tool.unpack_partition(corrupt)
        with self.assertRaises(ValueError):partition_tool.unpack_partition(storage[:-1])

    def test_production_storage_and_receiver(self):
        with tempfile.TemporaryDirectory(prefix='amity-partition-test-') as directory:
            base=Path(directory)
            (base/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_INVALID_ARG=3,ESP_ERR_INVALID_CRC=4;\n')
            (base/'esp_partition.h').write_text('#pragma once\n#include "esp_err.h"\n#include <cstddef>\nstruct esp_partition_t{size_t size;unsigned index;};\n')
            sources=[]
            for source in ('partition_image.cpp','peer_firmware_update.cpp'):
                sources.append('\n'.join(line for line in (ROOT/'components/system_services'/source).read_text().splitlines() if not line.startswith('#include')))
            cpp=base/'test.cpp';cpp.write_text(PREFIX+'\n'.join(sources)+SUITE)
            include=[];link=[]
            if Path('/opt/homebrew/opt/openssl@3').exists():include=['-I/opt/homebrew/opt/openssl@3/include'];link=['-L/opt/homebrew/opt/openssl@3/lib']
            command=shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-I'+str(base),'-I'+str(ROOT/'components/system_services/include')]+include+[str(cpp)]+link+['-lcrypto','-o',str(base/'test')]
            subprocess.run(command,check=True);subprocess.run([str(base/'test')],check=True)


if __name__=='__main__':unittest.main()
