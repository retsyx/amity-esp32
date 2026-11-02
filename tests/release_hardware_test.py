#!/usr/bin/env python3
"""Cross-check compiled board descriptors, image manifests and GPIO-map fences."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import hardware_profile as hardware
import release_manifest as release


def development():
    values = {'AMITY_BOARD_LEGACY_1T': 'n', 'AMITY_BOARD_DEVELOPMENT_4T': 'y', 'AMITY_BOARD_RESET_GPIO': '-1'}
    for slot in range(4):
        for key in ('FRONT_GPIO', 'BACK_GPIO'):
            values[f'AMITY_TENTACLE{slot}_{key}'] = '-1'
    return values


CPP = r'''
#include "release_manifest.hpp"
#include "release_hardware.hpp"
#include "amity/consortium/wire.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <iostream>
#include <iterator>
namespace config_store {
std::string sha256_hex(std::string_view value) {
    unsigned char bytes[32]; SHA256(reinterpret_cast<const unsigned char *>(value.data()),value.size(),bytes);
    constexpr char alphabet[]="0123456789abcdef"; std::string result(64,'0');
    for(size_t i=0;i<32;++i){result[2*i]=alphabet[bytes[i]>>4];result[2*i+1]=alphabet[bytes[i]&15];}
    return result;
}
}
int main(int argc,char **argv) {
    if(argc==1){std::cout<<system_services::updates::release_hardware_descriptor;return 0;}
    if(argc==2&&std::string(argv[1])=="--capabilities"){std::cout<<amity::consortium::known_capabilities;return 0;}
    std::ifstream file(argv[1]);std::string text((std::istreambuf_iterator<char>(file)),{}),error;
    system_services::updates::ReleaseManifest manifest;
    const bool accepted=system_services::updates::parse_release_manifest(text,manifest,error);
    if(accepted)std::cout<<manifest.hardware_profile<<'\n'<<manifest.hardware_mapping_sha256<<'\n';
    else std::cout<<error<<'\n';
    return accepted?0:1;
}
'''


class HardwareReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='amity-release-hardware-')
        cls.base = Path(cls.directory.name)
        (cls.base / 'test.cpp').write_text(CPP)
        cls.binaries = {}
        include = []; libraries = []
        if Path('/opt/homebrew/opt/openssl@3').exists():
            include = ['-I/opt/homebrew/opt/openssl@3/include']
            libraries = ['-L/opt/homebrew/opt/openssl@3/lib']
        sources = ['components/system_services/release_manifest.cpp', 'components/consortium/json.cpp',
                   'components/consortium/update.cpp', 'components/consortium/wire.cpp',
                   'components/consortium/owned_intent.cpp', 'components/consortium/transfer.cpp', 'components/consortium/settings.cpp', 'components/amity_system/model.cpp']
        for name, config in [('legacy', {'AMITY_BOARD_LEGACY_1T': 'y', 'AMITY_BOARD_DEVELOPMENT_4T': 'n'}), ('development', development())]:
            flags = ['-DCONFIG_' + key + '=' + ('1' if value == 'y' else '0' if value == 'n' else value) for key, value in config.items()]
            binary = cls.base / name
            subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                           ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined'] + include + flags +
                           ['-I' + str(ROOT / path) for path in ['components/system_services/include', 'components/consortium/include', 'components/amity_system/include']] +
                           [str(ROOT / path) for path in sources] + [str(cls.base / 'test.cpp')] + libraries + ['-lcrypto', '-o', str(binary)], check=True)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def image(self, name, config):
        image = bytearray(288)
        image[0] = 0xe9; image[32:36] = bytes.fromhex('3254cdab'); image[48:54] = b'0.1.59'
        path = self.base / name
        path.write_bytes(image + hardware.descriptor(config))
        return path

    def manifest(self, image, sdkconfig=None):
        with patch.object(release, 'git', side_effect=lambda *args: 'a' * 40 if args[0] == 'rev-parse' else ''), patch.object(release, 'tree_identity', return_value='b' * 64):
            return release.manifest(image, sdkconfig=sdkconfig)

    def accepts(self, binary, value):
        path = self.base / 'candidate.json'; path.write_bytes(release.canonical(value))
        result = subprocess.run([str(self.binaries[binary]), str(path)], capture_output=True, text=True)
        self.assertIn(result.returncode, [0, 1], result.stderr)
        return result.returncode == 0

    def test_compiled_marker_matches_independent_python_for_both_profiles(self):
        for name, config in [('legacy', {'AMITY_BOARD_LEGACY_1T': 'y'}), ('development', development())]:
            actual = subprocess.check_output([str(self.binaries[name])])
            self.assertEqual(actual, hardware.descriptor(config))
            self.assertEqual(hardware.identity(actual)[0], hardware.LEGACY if name == 'legacy' else hardware.DEVELOPMENT)

    def test_generated_capabilities_match_compiled_protocol(self):
        for name, config in [('legacy', {'AMITY_BOARD_LEGACY_1T': 'y'}), ('development', development())]:
            known = int(subprocess.check_output([str(self.binaries[name]), '--capabilities']))
            protocol = self.manifest(self.image(name + '-capabilities.bin', config))['protocol']
            self.assertEqual(int(protocol['optional_capabilities']), known)
            self.assertEqual(int(protocol['required_capabilities']) & ~known, 0)

    def test_exact_profile_and_mapping_required_before_admission(self):
        for name, config in [('legacy', {'AMITY_BOARD_LEGACY_1T': 'y'}), ('development', development())]:
            value = self.manifest(self.image(name + '.bin', config))
            self.assertTrue(self.accepts(name, value))
            self.assertFalse(self.accepts('development' if name == 'legacy' else 'legacy', value))
            for profiles in [[], ['unknown'], value['hardware_profiles'] * 2, [hardware.LEGACY, hardware.DEVELOPMENT]]:
                self.assertFalse(self.accepts(name, value | {'hardware_profiles': profiles}))
            if name == 'development':
                self.assertEqual(value['hardware_mapping_sha256'], hashlib.sha256(hardware.descriptor(config)).hexdigest())
                absent = dict(value); absent.pop('hardware_mapping_sha256')
                self.assertFalse(self.accepts(name, absent))
                self.assertFalse(self.accepts(name, value | {'hardware_mapping_sha256': '0' * 64}))
                changed = dict(config); changed['AMITY_TENTACLE3_FRONT_GPIO'] = '1'; changed['AMITY_TENTACLE3_BACK_GPIO'] = '2'
                self.assertFalse(self.accepts(name, value | {'hardware_mapping_sha256': hashlib.sha256(hardware.descriptor(changed)).hexdigest()}))
            else:
                self.assertNotIn('hardware_mapping_sha256', value)
                self.assertFalse(self.accepts(name, value | {'hardware_mapping_sha256': '0' * 64}))

    def test_generator_rejects_stale_config_and_unknown_or_ambiguous_images(self):
        config = development(); image = self.image('checks.bin', config)
        sdkconfig = self.base / 'sdkconfig.json'
        sdkconfig.write_text(json.dumps({key: value == 'y' if value in ['y', 'n'] else int(value) for key, value in config.items()}))
        self.assertEqual(self.manifest(image, sdkconfig)['hardware_profiles'], [hardware.DEVELOPMENT])
        sdkconfig.write_text(json.dumps({'AMITY_BOARD_LEGACY_1T': True}))
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.manifest(image, sdkconfig)
        raw = image.read_bytes()
        for data in [raw[:288], raw + hardware.descriptor(config), raw.replace(b'development-4t-v1', b'development-4t-x0'), raw.replace(b'capacity=4', b'capacity=5')]:
            image.write_bytes(data)
            with self.assertRaises(ValueError):
                self.manifest(image)

    def test_explicit_sdk_selection_and_gpio_validation(self):
        for config in [{}, {'AMITY_BOARD_LEGACY_1T': 'y', 'AMITY_BOARD_DEVELOPMENT_4T': 'y'}]:
            with self.assertRaises(ValueError): hardware.descriptor(config)
        for key, value in [('AMITY_TENTACLE0_FRONT_GPIO', '1'), ('AMITY_BOARD_RESET_GPIO', '19'), ('AMITY_BOARD_RESET_GPIO', '24'), ('AMITY_BOARD_DEVELOPMENT_4T', 'invalid')]:
            config = development(); config[key] = value
            with self.assertRaises(ValueError): hardware.descriptor(config)
        config = development(); config.pop('AMITY_TENTACLE0_FRONT_GPIO')
        with self.assertRaises(ValueError): hardware.descriptor(config)

    def test_slot_identity_contains_only_the_two_cec_wires(self):
        config = development()
        config.update(AMITY_TENTACLE2_FRONT_GPIO='7', AMITY_TENTACLE2_BACK_GPIO='8')
        raw = hardware.descriptor(config)
        self.assertIn(b'slot2=7,8\n', raw)
        malformed = raw.replace(b'slot2=7,8\n', b'slot2=7,8,41,0,1,0\n')
        with self.assertRaisesRegex(ValueError, 'slot descriptor'):
            hardware.identity(malformed)


if __name__ == '__main__':
    unittest.main()
