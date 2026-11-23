#!/usr/bin/env python3
"""Test the update bundle packaging tool and the release scripts."""
import hashlib
import json
import os
from pathlib import Path
import struct
import zipfile
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
# The tools run without site-packages whenever this test does.
PYTHON=[sys.executable]+(['-S'] if sys.flags.no_site else [])
sys.path.insert(0,str(ROOT/'tools'))
import update_bundle as bundle
import hardware_profile as hardware

class BundleTests(unittest.TestCase):
    def image_manifest(self):
        image=bytearray(288);image[0]=0xe9;image[32:36]=bytes.fromhex('3254cdab');image[176:208]=bytes(range(32));image+=hardware.descriptor({'AMITY_BOARD_LEGACY_1T':'y','AMITY_BOARD_DEVELOPMENT_4T':'n'})
        value={'image_bytes':str(len(image)),'image_sha256':hashlib.sha256(image).hexdigest(),'image_elf_sha256':image[176:208].hex(),'signing_key_id':'','hardware_profiles':['esp32s3-legacy-1t-v1']}
        return bytes(image),value
    def canonical(self,value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
    def test_tool_exact_native_and_signed_contract(self):
        image,value=self.image_manifest();manifest=self.canonical(value);file=bundle.bundle(image,manifest);ml,sl,il,offset=bundle.parse_header(file[:32],len(file));self.assertEqual((ml,sl,il),(len(manifest),0,len(image)));self.assertEqual(file[offset:],image)
        value['signing_key_id']='a'*64
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
            command=[*PYTHON,str(ROOT/'tools/update_bundle.py'),'--image',str(native),'--manifest',str(manifest)]
            subprocess.run(command,check=True,capture_output=True);produced=output.read_bytes();offset=bundle.parse_header(produced[:32],len(produced))[3];self.assertEqual(produced[offset:],image);self.assertEqual(native.read_bytes(),image)
            value['image_sha256']='0'*64;manifest.write_bytes(self.canonical(value));failure=subprocess.run(command,capture_output=True);self.assertNotEqual(failure.returncode,0);self.assertEqual(output.read_bytes(),produced);self.assertEqual(native.read_bytes(),image)
    def test_actual_release_packaging_and_publish_arguments(self):
        package=[*PYTHON,str(ROOT/'tools/release_assets.py')]
        publish=['bash',str(ROOT/'tools/publish_release.sh')]
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
                subprocess.run(package,cwd=base,env=env,check=True)
                assets=build/'release';checksums=(assets/'SHA256SUMS').read_text()
                for name in ('amity-update.bin','amity.release.json')+ (('amity.release.sig',) if signed else ()):
                    self.assertEqual((assets/name).read_bytes(),(build/name).read_bytes());self.assertIn(hashlib.sha256((assets/name).read_bytes()).hexdigest()+'  '+name,checksums)
                (base/'dist').symlink_to(assets,target_is_directory=True)
                commands=base/'commands';commands.mkdir();gh=commands/'gh'
                gh.write_text('#!'+sys.executable+'\nimport json,os,sys\nopen(os.environ["ARGS_FILE"],"w").write(json.dumps(sys.argv[1:]))\n');gh.chmod(0o700)
                env.update(PATH=str(commands)+os.pathsep+os.environ['PATH'],ARGS_FILE=str(base/'args.json'),GH_REPO='test/amity')
                subprocess.run(publish,cwd=base,env=env,check=True)
                arguments=json.loads((base/'args.json').read_text());self.assertEqual(arguments[:3],['release','create','v0.1.61'])
                self.assertIn('dist/amity-update.bin',arguments);self.assertIn('dist/amity.release.json',arguments);self.assertEqual('dist/amity.release.sig' in arguments,signed)
                (build/'amity.release.json').unlink()
                failure=subprocess.run(package,cwd=base,env=env,capture_output=True)
                self.assertNotEqual(failure.returncode,0) # Missing metadata cannot silently publish image-only assets.

if __name__=='__main__':unittest.main()
