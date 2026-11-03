#!/usr/bin/env python3
"""Keep the validated release envelope in the application's OTA partition."""
import argparse
import hashlib
from pathlib import Path
import struct

from update_bundle import bundle

SLOT_BYTES=0x3d0000
RELEASE_BYTES=0x3000
MAGIC=b'AMITY-IMAGE-V1\n\0'
HEADER=struct.Struct('>16sIIQ32s')


def partition_image(image,manifest,signature=b''):
    # Reuse the exact image/manifest/bundle binding checks. The metadata trailer
    # is outside the native image and outside its signed/hash-covered byte range.
    bundle(image,manifest,signature)
    if len(image)>SLOT_BYTES-RELEASE_BYTES:
        raise ValueError('Application image leaves no space for its release information')
    payload=manifest+signature
    trailer=HEADER.pack(MAGIC,len(manifest),len(signature),len(image),hashlib.sha256(payload).digest())+payload
    if len(trailer)>RELEASE_BYTES:raise ValueError('Release information exceeds its reserved partition area')
    return image+b'\xff'*(SLOT_BYTES-RELEASE_BYTES-len(image))+trailer+b'\xff'*(RELEASE_BYTES-len(trailer))


def unpack_partition(data):
    if len(data)!=SLOT_BYTES:raise ValueError('Expected a complete OTA partition image')
    trailer=data[-RELEASE_BYTES:]
    magic,manifest_bytes,signature_bytes,image_bytes,digest=HEADER.unpack(trailer[:HEADER.size])
    if magic!=MAGIC or not 1<=manifest_bytes<=8192 or signature_bytes not in (0,64) or not 1<=image_bytes<=SLOT_BYTES-RELEASE_BYTES:
        raise ValueError('Invalid partition release information')
    payload=trailer[HEADER.size:HEADER.size+manifest_bytes+signature_bytes]
    if hashlib.sha256(payload).digest()!=digest:raise ValueError('Incomplete partition release information')
    image=data[:image_bytes];manifest=payload[:manifest_bytes];signature=payload[manifest_bytes:]
    bundle(image,manifest,signature)
    return image,manifest,signature


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--manifest',required=True,type=Path)
    parser.add_argument('--signature',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    if args.output.resolve() in {args.image.resolve(),args.manifest.resolve()}:
        raise ValueError('Partition output must preserve its original release artifacts')
    data=partition_image(args.image.read_bytes(),args.manifest.read_bytes(),args.signature.read_bytes() if args.signature else b'')
    temporary=args.output.with_suffix(args.output.suffix+'.tmp');temporary.write_bytes(data);temporary.replace(args.output)
    print('OTA partition image:',args.output)


if __name__=='__main__':main()
