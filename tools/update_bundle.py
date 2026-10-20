#!/usr/bin/env python3
"""Package an existing release manifest/signature and unchanged OTA application.

The bounded one-file artifact is for the existing browser firmware picker. It
never creates compatibility metadata, signs an image, flashes, or provisions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

MAGIC=b'AMITY-UPDATE-V1\n'
HEADER=struct.Struct('>16sIIQ')
MAX_MANIFEST=8192
MAX_IMAGE=0x3d0000


def header(manifest_bytes,signature_bytes,image_bytes):
    if not 1<=manifest_bytes<=MAX_MANIFEST or signature_bytes not in (0,64) or not 1<=image_bytes<=MAX_IMAGE:
        raise ValueError('Update bundle lengths exceed the supported profile')
    return HEADER.pack(MAGIC,manifest_bytes,signature_bytes,image_bytes)


def parse_header(data,file_bytes):
    if len(data)!=HEADER.size:raise ValueError('Invalid update bundle header')
    magic,manifest_bytes,signature_bytes,image_bytes=HEADER.unpack(data)
    if magic!=MAGIC:raise ValueError('Invalid update bundle magic')
    header(manifest_bytes,signature_bytes,image_bytes)
    offset=HEADER.size+manifest_bytes+signature_bytes
    if offset+image_bytes!=file_bytes:raise ValueError('Update bundle is truncated or has trailing data')
    return manifest_bytes,signature_bytes,image_bytes,offset


def unique_object(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError('Duplicate manifest field')
        result[key]=value
    return result


def bundle(image,manifest,signature=b''):
    prefix=header(len(manifest),len(signature),len(image))
    value=json.loads(manifest,object_pairs_hook=unique_object)
    if json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()!=manifest:
        raise ValueError('Use the exact canonical release manifest')
    if not isinstance(value,dict) or value.get('security_profile') not in ('development','production'):
        raise ValueError('Use a supported release security profile')
    if value.get('image_bytes')!=str(len(image)) or value.get('image_sha256')!=hashlib.sha256(image).hexdigest():
        raise ValueError('Manifest differs from the exact native application image')
    if len(image)<288 or image[0]!=0xe9 or image[32:36]!=bytes.fromhex('3254cdab') or value.get('image_elf_sha256')!=image[176:208].hex():
        raise ValueError('Native application identity differs from the manifest')
    if (value.get('security_profile')=='production' or value.get('signing_key_id')) and len(signature)!=64:
        raise ValueError('This release requires its exact 64-byte publisher signature')
    if signature and not value.get('signing_key_id'):raise ValueError('Signature has no publisher identity')
    # Cross-check image-bound hardware metadata; never relabel it for a target.
    from hardware_profile import image_identity
    profile,mapping=image_identity(image)
    if value.get('hardware_profiles')!=[profile] or value.get('hardware_mapping_sha256','')!=mapping:
        raise ValueError('Manifest hardware identity differs from the compiled image')
    return prefix+manifest+signature+image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--signature',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args();output=args.output or args.image.with_name('amity-update.bin')
    if output.resolve() in (args.image.resolve(),args.manifest.resolve()) or (args.signature and output.resolve()==args.signature.resolve()):
        raise ValueError('Bundle output must preserve its original release artifacts')
    if not 1<=args.image.stat().st_size<=MAX_IMAGE or not 1<=args.manifest.stat().st_size<=MAX_MANIFEST:
        raise ValueError('Release artifact exceeds the supported profile')
    if args.signature and args.signature.stat().st_size!=64:
        raise ValueError('Publisher signature must contain exactly 64 bytes')
    data=bundle(args.image.read_bytes(),args.manifest.read_bytes(),args.signature.read_bytes() if args.signature else b'')
    temporary=None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent,prefix=output.name+'.',delete=False) as stream:
            temporary=Path(stream.name);stream.write(data);stream.flush();os.fsync(stream.fileno())
        os.chmod(temporary,0o644);os.replace(temporary,output);temporary=None
    finally:
        if temporary is not None:temporary.unlink(missing_ok=True)
    print('Browser update bundle:',output)


if __name__=='__main__':main()
