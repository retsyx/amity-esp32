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
import sys

sys.path.insert(0,str(Path(__file__).resolve().parent))
from hardware_profile import image_identity, read_config

def canonical(value):
    # This manifest's closed vocabulary uses ASCII keys/values and small integer
    # format tags; all counters are decimal strings, so this is its JCS encoding.
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()

def manifest(image,signing_key_id='',sdkconfig=None):
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
    digest=hashlib.sha256(data).hexdigest()
    # Firmware admits release identifiers of [A-Za-z0-9._-] only. Semver build
    # metadata keeps its '+' in firmware_version.
    release_id='amity-'+version.replace('+','_')+'-'+digest[:12]
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,96}',release_id):raise ValueError('Application version cannot form a release identifier')
    value=dict(format=1,release_id=release_id,firmware_version=version,
        image_bytes=str(len(data)),image_sha256=digest,image_elf_sha256=data[176:208].hex(),
        hardware_profiles=[hardware],signing_key_id=signing_key_id)
    if mapping:value['hardware_mapping_sha256']=mapping
    return value

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True);parser.add_argument('--output',type=Path)
    parser.add_argument('--sdkconfig',type=Path,help='Cross-check compiled image identity against this exact build configuration')
    parser.add_argument('--signing-key',type=Path,help='Publisher key; production firmware accepts only releases it signed')
    args=parser.parse_args();key=None;key_id=''
    if args.signing_key:
        from cryptography.hazmat.primitives import serialization,hashes
        from cryptography.hazmat.primitives.asymmetric import ec,utils
        key=serialization.load_pem_private_key(args.signing_key.read_bytes(),password=None)
        if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):raise ValueError('Release key must be P-256')
        public=key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)
        key_id=hashlib.sha256(public).hexdigest()
    value=manifest(args.image,key_id,args.sdkconfig)
    data=canonical(value)
    output=args.output or args.image.with_suffix('.release.json');output.write_bytes(data)
    if key:
        r,s=utils.decode_dss_signature(key.sign(data,ec.ECDSA(hashes.SHA256())))
        output.with_suffix('.sig').write_bytes(r.to_bytes(32,'big')+s.to_bytes(32,'big'))
    else:
        output.with_suffix('.sig').unlink(missing_ok=True)
    print('Release manifest:',output,'(signed)' if key else '(unsigned)')

if __name__=='__main__':main()
