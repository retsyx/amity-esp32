#!/usr/bin/env python3
"""Test the actual bounded bundle parser/upload function and packaging tool."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import textwrap
import zipfile
import subprocess
import sys
import tempfile
import unittest
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import update_bundle as bundle
import hardware_profile as hardware

PREFIX=r'''
#include "update_bundle.hpp"
#include "release_manifest.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <openssl/sha.h>
using esp_err_t=int;using esp_ota_handle_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1,ESP_ERR_TIMEOUT=2,ESP_ERR_INVALID_CRC=3,HTTPD_400_BAD_REQUEST=400,HTTPD_500_INTERNAL_SERVER_ERROR=500;
constexpr int CONFIG_IDF_FIRMWARE_CHIP_ID=9,ESP_IMAGE_HEADER_MAGIC=0xe9;constexpr uint32_t ESP_APP_DESC_MAGIC_WORD=0xabcd5432u;
struct esp_image_header_t{uint8_t magic=ESP_IMAGE_HEADER_MAGIC;uint8_t rest[11]{};uint16_t chip_id=CONFIG_IDF_FIRMWARE_CHIP_ID;uint8_t end[10]{};};
struct esp_image_segment_header_t{uint32_t address=0,bytes=0;};
struct esp_app_desc_t{uint32_t magic_word=ESP_APP_DESC_MAGIC_WORD,secure=0,reserved[2]{};char version[32]="0.1.61",project_name[32]="amity",time[16]{},date[16]{},sdk[32]{};uint8_t app_elf_sha256[32]{};uint8_t tail[80]{};};
static_assert(sizeof(esp_image_header_t)==24&&sizeof(esp_app_desc_t)==256);
struct esp_partition_t{size_t size=0x3d0000;}partition;
struct httpd_req_t{std::string input,response;size_t content_len=0,cursor=0,fragment=4096,fail_after=SIZE_MAX;int status=0;};
esp_app_desc_t running;
unsigned erases=0,writes=0,selections=0,reboots=0,stages=0;bool admitted=false;
std::string native,written,expected_manifest="canonical-exact-manifest",expected_signature;
const esp_partition_t*esp_ota_get_next_update_partition(void*){return &partition;}
const esp_app_desc_t*esp_app_get_description(){return &running;}
const char*esp_err_to_name(int){return "fake failure";}
int64_t clock_us=0;int64_t esp_timer_get_time(){return ++clock_us;}
int httpd_req_recv(httpd_req_t*req,char*out,size_t count){if(req->cursor>=req->input.size()||req->cursor>=req->fail_after)return -1;count=std::min({count,req->fragment,req->input.size()-req->cursor,req->fail_after-req->cursor});memcpy(out,req->input.data()+req->cursor,count);req->cursor+=count;return static_cast<int>(count);}
int httpd_resp_send_err(httpd_req_t*req,int status,const char*message){req->status=status;req->response=message;return status;}
int httpd_resp_set_status(httpd_req_t*,const char*){return 0;}int httpd_resp_set_type(httpd_req_t*,const char*){return 0;}
int httpd_resp_sendstr(httpd_req_t*req,const char*text){req->response=text;return 0;}
int httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t){return ESP_FAIL;}
int esp_ota_begin(const esp_partition_t*,size_t size,esp_ota_handle_t*handle){assert(admitted&&size==native.size());++erases;*handle=1;return ESP_OK;}
int esp_ota_write(esp_ota_handle_t,const void*bytes,size_t count){++writes;written.append(static_cast<const char*>(bytes),count);return ESP_OK;}
int esp_ota_abort(esp_ota_handle_t){return ESP_OK;}int esp_ota_end(esp_ota_handle_t){return ESP_OK;}
int esp_ota_set_boot_partition(const esp_partition_t*){++selections;return ESP_OK;}
void reboot_later(){++reboots;}
std::string digest_hex(std::string_view input){unsigned char bytes[32];SHA256(reinterpret_cast<const unsigned char*>(input.data()),input.size(),bytes);std::string result;constexpr char hex[]="0123456789abcdef";for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}return result;}
struct psa_hash_operation_t{std::string input;};
#define PSA_HASH_OPERATION_INIT {}
constexpr int PSA_ALG_SHA_256=1,PSA_SUCCESS=0;
int psa_hash_setup(psa_hash_operation_t*,int){return 0;}
int psa_hash_update(psa_hash_operation_t*op,const void*bytes,size_t count){op->input.append(static_cast<const char*>(bytes),count);return 0;}
int psa_hash_finish(psa_hash_operation_t*op,uint8_t*out,size_t,size_t*count){SHA256(reinterpret_cast<const unsigned char*>(op->input.data()),op->input.size(),out);*count=32;return 0;}
int psa_hash_abort(psa_hash_operation_t*){return 0;}
namespace system_services {
std::optional<int> compare_firmware_versions(std::string_view,std::string_view){return 0;}
namespace updates{
std::optional<ReleaseManifest> staged;
std::optional<ReleaseManifest> staged_release(){return staged;}
int stage_release(std::string_view manifest,std::string_view signature,std::string&error){++stages;assert(!erases);if(manifest!=expected_manifest||signature!=expected_signature){error="Publisher/profile admission rejected";return ESP_FAIL;}ReleaseManifest value;value.image_bytes=native.size();value.image_sha256=digest_hex(native);value.firmware_version=running.version;for(auto b:running.app_elf_sha256){constexpr char hex[]="0123456789abcdef";value.image_elf_sha256+=hex[b>>4];value.image_elf_sha256+=hex[b&15];}staged=value;admitted=true;return ESP_OK;}
int begin_install(std::string&){assert(admitted);return ESP_OK;}void install_failed(const std::string&){}void install_progress(uint64_t){}int install_selected(){return ESP_OK;}
}
'''
SUITE=r'''
}
using namespace system_services;
std::string header(uint32_t manifest,uint32_t signature,uint64_t image){std::string out(updates::update_bundle_magic);auto add=[&](uint64_t value,unsigned bytes){for(unsigned n=bytes;n>0;--n)out+=char(value>>(8*(n-1)));};add(manifest,4);add(signature,4);add(image,8);assert(out.size()==32);return out;}
void reset(){erases=writes=selections=reboots=stages=0;admitted=false;written.clear();updates::staged.reset();expected_signature.clear();}
httpd_req_t request(std::string input,size_t fragment=4096){httpd_req_t req;req.input=std::move(input);req.content_len=req.input.size();req.fragment=fragment;return req;}
int main(int argc,char**argv){
 if(argc==2){std::ifstream stream(argv[1],std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(stream)),{}),error;updates::UpdateBundleHeader parsed;assert(updates::parse_update_bundle_header(std::string_view(bytes).substr(0,32),bytes.size(),parsed,error));assert(static_cast<unsigned char>(bytes[parsed.image_offset()])==0xe9);return 0;}
 native.resize(5000,'x');esp_image_header_t ih;esp_image_segment_header_t segment;memcpy(native.data(),&ih,sizeof(ih));memcpy(native.data()+24,&segment,8);for(unsigned n=0;n<32;++n)running.app_elf_sha256[n]=uint8_t(n+1);memcpy(native.data()+32,&running,sizeof(running));
 for(auto fragment:{1u,17u,31u,32u,255u,2048u}){
  reset();auto file=header(expected_manifest.size(),0,native.size())+expected_manifest+native;auto req=request(file,fragment);assert(upload_firmware(&req)==ESP_OK&&stages==1&&erases==1&&written==native&&selections==1&&reboots==1&&req.cursor==file.size());
 }
 reset();auto req=request(native,13);assert(upload_firmware(&req)==400&&!erases&&!stages);
 reset();std::string error;assert(updates::stage_release(expected_manifest,"",error)==ESP_OK);req=request(native,13);assert(upload_firmware(&req)==ESP_OK&&written==native&&erases==1);
 for(unsigned kind=0;kind<11;++kind){
  reset();auto manifest=expected_manifest;auto image=native;std::string signature;uint32_t ml=manifest.size(),sl=0;uint64_t il=image.size();
  switch(kind){case 0:ml=0;break;case 1:ml=8193;break;case 2:sl=63;break;case 3:il=UINT64_MAX;break;case 4:manifest[0]='!';break;case 5:sl=64;signature=std::string(64,'z');break;case 6:image[0]=0;break;case 7:image[48]='9';break;case 8:image[176]='!';break;case 9:il=0;break;case 10:il=0x3d0001;break;}
  req=request(header(ml,sl,il)+manifest+signature+image);assert(upload_firmware(&req)==400&&!erases&&!writes&&!selections);
 }
 for(int delta:{-1,1}){reset();auto file=header(expected_manifest.size(),0,native.size())+expected_manifest+native;if(delta<0)file.pop_back();else file+='!';req=request(file);assert(upload_firmware(&req)==400&&!stages&&!erases);}
 for(size_t fail:{size_t(2),size_t(33),size_t(50)}){reset();req=request(header(expected_manifest.size(),0,native.size())+expected_manifest+native);req.fail_after=fail;assert(upload_firmware(&req)==400&&!erases&&!selections);}
 reset();std::string signature(64,'z');for(unsigned char b:signature){constexpr char hex[]="0123456789abcdef";expected_signature+=hex[b>>4];expected_signature+=hex[b&15];}req=request(header(expected_manifest.size(),64,native.size())+expected_manifest+signature+native,7);assert(upload_firmware(&req)==ESP_OK&&written==native&&erases==1);
 reset();auto corrupt=native;corrupt.back()='!';req=request(header(expected_manifest.size(),0,corrupt.size())+expected_manifest+corrupt);assert(upload_firmware(&req)==400&&erases==1&&!selections&&!reboots); // Stream digest failure preserves active image.
 for(auto length:{0ull,1ull,8192ull,8193ull,0xffffffffull})for(auto signature_bytes:{0u,1u,64u,65u}){updates::UpdateBundleHeader parsed;const auto bytes=header(length,signature_bytes,native.size());const bool good=updates::parse_update_bundle_header(bytes,32+length+signature_bytes+native.size(),parsed,error);assert(good==(length>=1&&length<=8192&&(signature_bytes==0||signature_bytes==64)));}
 std::cout<<"PASS actual bundle header/upload: fragmented input, exact metadata before erase, signature admission, native bytes only, raw staged path, bounded lengths/truncation/trailing/header/hash rejection\n";
}
'''

class BundleTests(unittest.TestCase):
    def image_manifest(self):
        image=bytearray(288);image[0]=0xe9;image[32:36]=bytes.fromhex('3254cdab');image[176:208]=bytes(range(32));image+=hardware.descriptor({'AMITY_BOARD_LEGACY_1T':'y','AMITY_BOARD_DEVELOPMENT_5T':'n'})
        value={'image_bytes':str(len(image)),'image_sha256':hashlib.sha256(image).hexdigest(),'image_elf_sha256':image[176:208].hex(),'security_profile':'development','signing_key_id':'','hardware_profiles':['esp32s3-legacy-1t-v1']}
        return bytes(image),value
    def canonical(self,value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
    def test_tool_exact_native_and_signed_contract(self):
        image,value=self.image_manifest();manifest=self.canonical(value);file=bundle.bundle(image,manifest);ml,sl,il,offset=bundle.parse_header(file[:32],len(file));self.assertEqual((ml,sl,il),(len(manifest),0,len(image)));self.assertEqual(file[offset:],image)
        value['security_profile']='production';value['signing_key_id']='a'*64
        with self.assertRaises(ValueError):bundle.bundle(image,self.canonical(value))
        signed=bundle.bundle(image,self.canonical(value),b'z'*64);self.assertEqual(bundle.parse_header(signed[:32],len(signed))[1],64)
    def test_tool_rejects_wrong_or_noncanonical_artifacts(self):
        image,value=self.image_manifest()
        for field,replacement in [('image_bytes','1'),('image_sha256','a'*64),('image_elf_sha256','a'*64),('hardware_profiles',['unknown'])]:
            bad=dict(value);bad[field]=replacement
            with self.assertRaises(ValueError,msg=field):bundle.bundle(image,self.canonical(bad))
        with self.assertRaises(ValueError):bundle.bundle(image,json.dumps(value).encode())
        with self.assertRaises(ValueError):bundle.bundle(image,self.canonical(value),b'z'*63)
        with self.assertRaises(ValueError):bundle.bundle(image,self.canonical(value),b'z'*64)
    def test_cli_preserves_native_artifacts_and_previous_output_on_rejection(self):
        with tempfile.TemporaryDirectory(prefix='amity-bundle-cli-') as directory:
            base=Path(directory);image,value=self.image_manifest();native=base/'amity.bin';manifest=base/'amity.release.json';output=base/'amity-update.bin';native.write_bytes(image);manifest.write_bytes(self.canonical(value))
            command=[sys.executable,str(ROOT/'tools/update_bundle.py'),'--image',str(native),'--manifest',str(manifest)]
            subprocess.run(command,check=True,capture_output=True);produced=output.read_bytes();offset=bundle.parse_header(produced[:32],len(produced))[3];self.assertEqual(produced[offset:],image);self.assertEqual(native.read_bytes(),image)
            value['image_sha256']='0'*64;manifest.write_bytes(self.canonical(value));failure=subprocess.run(command,capture_output=True);self.assertNotEqual(failure.returncode,0);self.assertEqual(output.read_bytes(),produced);self.assertEqual(native.read_bytes(),image)
    def test_actual_release_packaging_and_publish_arguments(self):
        workflow=(ROOT/'.github/workflows/build-firmware.yml').read_text()
        block=workflow.split('- name: Prepare downloadable release assets',1)[1].split("python3 - <<'PY'",1)[1].split('\n          PY',1)[0]
        package_script=textwrap.dedent(block).lstrip()
        publish=(ROOT/'.github/workflows/release-firmware.yml').read_text().split('        run: |',1)[1]
        publish_script=textwrap.dedent(publish)
        for signed in (False,True):
            with tempfile.TemporaryDirectory(prefix='amity-release-assets-') as directory:
                base=Path(directory);build=base/'build';build.mkdir();(build/'amity-provision').mkdir()
                for name in ('amity.bin','amity-update.bin','amity.release.json','amity.elf','amity.map'):(build/name).write_bytes(name.encode())
                if signed:(build/'amity.release.sig').write_bytes(b'z'*64)
                with zipfile.ZipFile(build/'amity-firmware.zip','w') as archive:archive.writestr('manifest.json',json.dumps({'firmware_version':'0.1.61'}))
                for system in ('darwin','windows','linux'):
                    for arch in ('amd64','arm64'):(build/'amity-provision'/f'amity-provision-{system}-{arch}.zip').write_bytes(b'fake provisioner')
                (base/'README.md').write_text('Use the release update file. https://github.com/retsyx/amity-esp32/blob/main/README.md')
                env=dict(os.environ,RELEASE_TAG='v0.1.61')
                subprocess.run([sys.executable,'-c',package_script],cwd=base,env=env,check=True)
                assets=build/'release';checksums=(assets/'SHA256SUMS').read_text()
                for name in ('amity-update.bin','amity.release.json')+ (('amity.release.sig',) if signed else ()):
                    self.assertEqual((assets/name).read_bytes(),(build/name).read_bytes());self.assertIn(hashlib.sha256((assets/name).read_bytes()).hexdigest()+'  '+name,checksums)
                (base/'dist').symlink_to(assets,target_is_directory=True)
                commands=base/'commands';commands.mkdir();gh=commands/'gh'
                gh.write_text('#!'+sys.executable+'\nimport json,os,sys\nopen(os.environ["ARGS_FILE"],"w").write(json.dumps(sys.argv[1:]))\n');gh.chmod(0o700)
                env.update(PATH=str(commands)+os.pathsep+os.environ['PATH'],ARGS_FILE=str(base/'args.json'),GH_REPO='test/amity')
                subprocess.run(['bash','-euc',publish_script],cwd=base,env=env,check=True)
                arguments=json.loads((base/'args.json').read_text());self.assertEqual(arguments[:3],['release','create','v0.1.61'])
                self.assertIn('dist/amity-update.bin',arguments);self.assertIn('dist/amity.release.json',arguments);self.assertEqual('dist/amity.release.sig' in arguments,signed)
                (build/'amity.release.json').unlink()
                failure=subprocess.run([sys.executable,'-c',package_script],cwd=base,env=env,capture_output=True)
                self.assertNotEqual(failure.returncode,0) # Missing metadata cannot silently publish image-only assets.
    def test_actual_target_parser_and_upload(self):
        with tempfile.TemporaryDirectory(prefix='amity-bundle-test-') as directory:
            cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
            cpp.write_text(PREFIX+function((ROOT/'components/system_services/system_services.cpp').read_text(),'esp_err_t upload_firmware(httpd_req_t *req)')+SUITE)
            include=[];link=[]
            if Path('/opt/homebrew/opt/openssl@3').exists():include=['-I/opt/homebrew/opt/openssl@3/include'];link=['-L/opt/homebrew/opt/openssl@3/lib']
            subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I'+str(ROOT/'components/system_services/include')]+include+[str(cpp)]+link+['-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
            image,value=self.image_manifest();file=Path(directory)/'python-bundle.bin';file.write_bytes(bundle.bundle(image,self.canonical(value)));subprocess.run([str(binary),str(file)],check=True)

if __name__=='__main__':unittest.main()
