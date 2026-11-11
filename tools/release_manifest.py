#!/usr/bin/env python3
"""Create the canonical OTA release manifest; optionally sign with a release P-256 key.

The application image is an OTA artifact. This never flashes, provisions keys,
changes a partition table or programs secure boot/encryption fuses.
Hardware compatibility comes from this image's compiled public GPIO descriptor;
adjacent build metadata or --sdkconfig is checked, never used to relabel an image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0,str(Path(__file__).resolve().parent))
from hardware_profile import image_identity, read_config

ROOT=Path(__file__).resolve().parents[1]
DEPENDENCIES=dict(esp_idf='fff9895c82d744c7237be8847347bdd1b07c6643',
    esp_matter='59574f3fb62fddc690b4127fd3a4a43cfce4245e',
    connectedhomeip='539342f32d5f4dc93761c2f9325afe29270068f1',homekit='676fabac4a4a05184be020611cb069faa0016411')

def canonical(value):
    # This manifest's closed vocabulary uses ASCII keys/values and small integer
    # format tags; all counters are decimal strings, so this is its JCS encoding.
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()

def git(*arguments):return subprocess.check_output(['git',*arguments],cwd=ROOT).decode().strip()

def tree_identity():
    names=subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=ROOT).split(b'\0')
    digest=hashlib.sha256()
    for name in sorted(set(names)):
        if not name:continue
        path=ROOT/name.decode()
        if not path.is_file() or path.is_symlink():continue
        if name.startswith((b'hw/case/',b'test-results/',b'.cache/',b'build/')):continue
        digest.update(name+b'\0'+hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()

def manifest(image,profile='development',signing_key_id='',sdkconfig=None):
    data=Path(image).read_bytes()
    if len(data)<288 or data[0]!=0xe9 or data[32:36]!=bytes.fromhex('3254cdab'):
        raise ValueError('Expected an ESP32-S3 application image')
    version=data[48:80].split(b'\0')[0].decode('ascii')
    if not version or len(version)>31:raise ValueError('Invalid application version')
    # The embedded descriptor binds this exact image. Build metadata is checked
    # when available; a source-tree sdkconfig is never guessed for an artifact.
    metadata=Path(sdkconfig) if sdkconfig else Path(image).parent/'config'/'sdkconfig.json'
    hardware,mapping=image_identity(data,read_config(metadata) if metadata.exists() else None)
    if sdkconfig and not metadata.exists():raise ValueError('Explicit SDK configuration does not exist')
    commit=git('rev-parse','HEAD')
    dirty=bool(git('status','--porcelain','--untracked-files=normal'))
    formats=lambda low,high,write:dict(read_min=str(low),read_max=str(high),write=str(write))
    value=dict(format=1,release_id='amity-'+version+'-'+hashlib.sha256(data).hexdigest()[:12],firmware_version=version,
        image_bytes=str(len(data)),image_sha256=hashlib.sha256(data).hexdigest(),image_elf_sha256=data[176:208].hex(),
        chip='esp32s3',hardware_profiles=[hardware],partition_layout='amity-8m-ota3d0000-v1',slot_bytes=str(0x3d0000),
        protocol=dict(min='1',max='1',required_capabilities='401472',optional_capabilities='2089544'),
        configuration=formats(3,3,3),storage=formats(2,2,2),origin=formats(1,1,1),factory=formats(1,4,4),
        bootloader='esp-idf6.1-rollback-v1',security_profile=profile,rollback_readable_max='2',source_commit=commit,
        source_tree_sha256=tree_identity(),source_dirty=dirty,dependencies=DEPENDENCIES,signing_key_id=signing_key_id)
    if mapping:value['hardware_mapping_sha256']=mapping
    return value

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True);parser.add_argument('--output',type=Path)
    parser.add_argument('--sdkconfig',type=Path,help='Cross-check compiled image identity against this exact build configuration')
    parser.add_argument('--compatibility-capabilities',type=lambda text:int(text,0),help='Optional capability ceiling for an older installer; never changes image bytes or the required floor');parser.add_argument('--signing-key',type=Path);parser.add_argument('--profile',choices=['development','production'],default='development')
    args=parser.parse_args();key=None;key_id=''
    if args.signing_key:
        from cryptography.hazmat.primitives import serialization,hashes
        from cryptography.hazmat.primitives.asymmetric import ec,utils
        key=serialization.load_pem_private_key(args.signing_key.read_bytes(),password=None)
        if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):raise ValueError('Release key must be P-256')
        public=key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)
        key_id=hashlib.sha256(public).hexdigest()
    if args.profile=='production' and key is None:raise ValueError('Production release requires a publisher signing key')
    value=manifest(args.image,args.profile,key_id,args.sdkconfig)
    if args.compatibility_capabilities is not None:
        ceiling=args.compatibility_capabilities
        if ceiling<0 or ceiling>0xffffffff or int(value['protocol']['required_capabilities'])&~ceiling:raise ValueError('Compatibility ceiling excludes the required protocol floor')
        value['protocol']['optional_capabilities']=str(int(value['protocol']['optional_capabilities'])&ceiling)
    data=canonical(value)
    output=args.output or args.image.with_suffix('.release.json');output.write_bytes(data)
    if key:
        r,s=utils.decode_dss_signature(key.sign(data,ec.ECDSA(hashes.SHA256())))
        output.with_suffix('.sig').write_bytes(r.to_bytes(32,'big')+s.to_bytes(32,'big'))
    else:
        output.with_suffix('.sig').unlink(missing_ok=True)
    print('Release manifest:',output,'('+args.profile+(' signed' if key else ' unsigned')+')')

if __name__=='__main__':main()
