#!/usr/bin/env python3
"""Exercise actual HTTPS identity generation with pinned Mbed TLS and OpenSSL.

Only random-fill and configuration storage are seams. Deterministic draws cover
the leading-zero serial failure, and saved identities are checked for repair or
byte-for-byte reuse. OpenSSL independently parses every resulting certificate.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from consortium_tls_credentials_test import native_library

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=22;
namespace config_store {
std::map<std::string,std::string> records;
esp_err_t read(const char *key,std::string &out){
    const auto found=records.find(key);
    if(found==records.end())return ESP_ERR_NVS_NOT_FOUND;
    out=found->second;return ESP_OK;
}
esp_err_t write(const char *key,const std::string &value){records[key]=value;return ESP_OK;}
}
std::array<uint8_t,16> draw{};
unsigned draws=0;
void esp_fill_random(void *out,size_t size){assert(size==draw.size());std::memcpy(out,draw.data(),size);++draws;}
namespace web {
std::string certificate,private_key;
'''
SUITE = r'''
}
// Build signed saved-certificate fixtures with the real raw serial API. Mbed TLS
// permits the redundant zeros that OpenSSL rejects, so parsing alone is no fence.
std::string fixture_certificate(const std::string &pem,const uint8_t *serial,size_t size){
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    assert(!mbedtls_pk_parse_key(&key,reinterpret_cast<const uint8_t *>(pem.c_str()),pem.size()+1,nullptr,0));
    mbedtls_x509write_cert cert;mbedtls_x509write_crt_init(&cert);
    mbedtls_x509write_crt_set_version(&cert,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert,&key);
    mbedtls_x509write_crt_set_issuer_key(&cert,&key);
    assert(!mbedtls_x509write_crt_set_serial_raw(&cert,serial,size));
    assert(!mbedtls_x509write_crt_set_subject_name(&cert,"CN=amity,O=Amity"));
    assert(!mbedtls_x509write_crt_set_issuer_name(&cert,"CN=amity,O=Amity"));
    assert(!mbedtls_x509write_crt_set_validity(&cert,"20240101000000","20500101000000"));
    uint8_t result[1600]{};assert(!mbedtls_x509write_crt_pem(&cert,result,sizeof(result)));
    mbedtls_x509write_crt_free(&cert);mbedtls_pk_free(&key);
    return reinterpret_cast<char *>(result);
}
void emit(const std::string &root,const std::string &name,const std::string &pem){
    std::ofstream file(root+"/"+name+".pem");file<<pem;assert(file.good());
}
void canonical_serial(const std::string &pem){
    mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);
    assert(!mbedtls_x509_crt_parse(&crt,reinterpret_cast<const uint8_t *>(pem.c_str()),pem.size()+1));
    const auto &s=crt.serial;
    assert(s.len && !(s.p[0]&0x80));
    assert(s.p[0]!=0 || (s.len>1 && (s.p[1]&0x80)));
    mbedtls_x509_crt_free(&crt);
}
int main(int argc,char **argv){
    assert(argc==2 && psa_crypto_init()==PSA_SUCCESS);
    for(unsigned scenario=0;scenario<5;++scenario){
        draw.fill(0);
        if(scenario==0)draw[1]=1; // Redundant zero prefix before a positive byte.
        if(scenario==1){draw[0]=0x80;draw[1]=1;} // Masking creates the same prefix.
        if(scenario==2)draw[1]=0x80; // A zero sign prefix would be necessary here.
        if(scenario==3)draw.fill(0xff);
        // Scenario 4 is an all-zero draw: serial must still be positive.
        config_store::records.clear();
        assert(web::tls_identity("amity")==ESP_OK);
        canonical_serial(web::certificate);
        emit(argv[1],"generated-"+std::to_string(scenario),web::certificate);
        const auto saved=config_store::records;
        const auto before=draws;
        assert(web::tls_identity("amity")==ESP_OK);
        assert(draws==before && config_store::records==saved);
    }
    const uint8_t redundant[]={0,1},zero[]={0},positive[]={0x80};
    for(unsigned scenario=0;scenario<3;++scenario){
        const auto bad=scenario==0?fixture_certificate(web::private_key,redundant,sizeof(redundant)):
            scenario==1?fixture_certificate(web::private_key,zero,sizeof(zero)):"not a certificate";
        config_store::records["tls_cert"]=bad;
        const auto old_key=config_store::records.at("tls_key");
        assert(web::tls_identity("amity")==ESP_OK);
        assert(config_store::records.at("tls_cert")!=bad && config_store::records.at("tls_key")!=old_key);
        canonical_serial(web::certificate);
        emit(argv[1],"repaired-"+std::to_string(scenario),web::certificate);
    }
    // A necessary leading zero is valid DER and must preserve the saved identity.
    const auto valid=fixture_certificate(web::private_key,positive,sizeof(positive));
    config_store::records["tls_cert"]=valid;
    const auto saved=config_store::records;
    const auto before=draws;
    assert(web::tls_identity("amity")==ESP_OK);
    assert(draws==before && config_store::records==saved && web::certificate==valid);
    canonical_serial(valid);emit(argv[1],"preserved",valid);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/web/web.cpp')
    firmware = parser.parse_args().source.read_text()
    actual = ''.join(function(firmware, name) for name in
                     ('bool valid_certificate_serial(', 'esp_err_t tls_identity(')
                     if name in firmware)
    source, build = native_library()
    with tempfile.TemporaryDirectory(prefix='amity-web-tls-') as directory:
        path = Path(directory)
        cpp, binary = path / 'test.cpp', path / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        include = [source / 'include', source / 'tf-psa-crypto/include',
                   source / 'tf-psa-crypto/drivers/builtin/include', build / 'tf-psa-crypto/include']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            *['-I' + str(item) for item in include], str(cpp),
            *[str(build / 'library' / name) for name in
              ('libmbedtls.a', 'libmbedx509.a', 'libtfpsacrypto.a')], '-o', str(binary)], check=True)
        subprocess.run([str(binary), directory], check=True)
        certificates = list(path.glob('*.pem'))
        assert len(certificates) == 9
        for certificate in certificates:
            subprocess.run(['openssl', 'x509', '-in', str(certificate), '-noout', '-serial'],
                           check=True, stdout=subprocess.DEVNULL)
    print('PASS actual HTTPS identity: canonical positive serials, malformed saved identity repair, '
          'valid identity reuse; pinned Mbed TLS and OpenSSL')


if __name__ == '__main__':
    main()
