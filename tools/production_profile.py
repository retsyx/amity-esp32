#!/usr/bin/env python3
"""Prepare and audit an offline production build profile. Never flashes or burns fuses.

Publisher verification uses P-256. The ESP32-S3 native secure-boot signing key is
separate and is supplied as a build-time path reference, never copied here.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import os
import zlib
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from hardware_profile import descriptor, identity, BOARD_FLAGS, read_config as read_board_config
REQUIRED={name:'y' for name in (
    'AMITY_PRODUCTION_UPDATES','SECURE_BOOT','SECURE_BOOT_V2_ENABLED',
    'SECURE_BOOT_BUILD_SIGNED_BINARIES','SECURE_FLASH_ENC_ENABLED',
    'SECURE_FLASH_ENCRYPTION_MODE_RELEASE','NVS_ENCRYPTION',
    'NVS_SEC_KEY_PROTECT_USING_FLASH_ENC','BOOTLOADER_APP_ROLLBACK_ENABLE')}
PREFIX=bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')


def publisher_der(path):
    data=Path(path).read_bytes()
    result=subprocess.run(['openssl','pkey','-pubin','-inform','PEM' if data.startswith(b'-----BEGIN') else 'DER','-outform','DER'],input=data,capture_output=True)
    if result.returncode or len(result.stdout)!=91 or not result.stdout.startswith(PREFIX):
        raise ValueError('Publisher verification key must be a P-256 public SPKI')
    return result.stdout


def read_config(path):
    return read_board_config(path)


def partitions(path):
    result={}
    for row in csv.reader(line for line in Path(path).read_text().splitlines() if line.strip() and not line.lstrip().startswith('#')):
        if len(row)!=6:raise ValueError('Expected the explicit six-column partition layout')
        name,kind,subtype,offset,size,flags=(field.strip() for field in row)
        if name in result:raise ValueError('Duplicate partition name')
        result[name]=dict(type=kind,subtype=subtype,offset=int(offset,0),size=int(size,0),encrypted='encrypted' in flags.split(':'))
    return result


def audit(config,table,key):
    errors=[]
    hardware_profile='';hardware_mapping=''
    try:hardware_profile,hardware_mapping=identity(descriptor(config))
    except ValueError as error:errors.append('Invalid compiled hardware profile: '+str(error))
    for name,value in REQUIRED.items():
        if config.get(name)!=value:errors.append('CONFIG_'+name+' must be '+value)
    for name in ('SECURE_BOOT_ALLOW_JTAG','SECURE_BOOT_INSECURE','SECURE_BOOT_ALLOW_ROM_BASIC','SECURE_FLASH_UART_BOOTLOADER_ALLOW_ENC','SECURE_FLASH_UART_BOOTLOADER_ALLOW_DEC','SECURE_FLASH_UART_BOOTLOADER_ALLOW_CACHE'):
        if config.get(name)=='y':errors.append('CONFIG_'+name+' enables an unsupported production debug path')
    if config.get('AMITY_RELEASE_PUBLIC_KEY_DER_HEX')!=json.dumps(key.hex()):errors.append('Publisher verification root does not match the provisioned public SPKI')
    expected=partitions(ROOT/'partitions.csv')
    if set(table)!=set(expected):errors.append('Partition inventory differs from the supported layout')
    for name,entry in expected.items():
        current=table.get(name)
        if current is None:continue
        if any(current[field]!=entry[field] for field in ('type','subtype','offset','size')):errors.append('Unsupported partition geometry: '+name)
    for name in ('amity_factory','nvs_keys','esp_secure_cert'):
        if not table.get(name,{}).get('encrypted'):errors.append('Flash encryption flag required: '+name)
    for name in ('amity_cfg','nvs','fctry'):
        if table.get(name,{}).get('encrypted'):errors.append('NVS uses its own secure initialization, not raw flash encryption: '+name)
    return dict(profile_valid=not errors,errors=errors,security_profile='production',hardware_profile=hardware_profile,hardware_mapping_sha256=hardware_mapping,publisher_key_id=hashlib.sha256(key).hexdigest(),native_boot_verification='requires signed-image and provisioned-device evidence',nvs_key_policy='existing SDK NVS root; HKDF-SHA256 v1 per-partition keys for amity_cfg/fctry; no automatic replacement',production_qualified=False,device_mutations=False)


def prepare(key,native_reference,out,board_config=None):
    if not native_reference or any(c in native_reference for c in '\n\r"'):
        raise ValueError('Supply the separate native signing-key path reference')
    out.mkdir(parents=True,exist_ok=True)
    table=out/'partitions.production.csv'
    rows=[]
    for line in (ROOT/'partitions.csv').read_text().splitlines():
        if line.startswith('amity_factory,'):
            columns=line.split(',');columns[-1]=' encrypted';line=','.join(columns)
        rows.append(line)
    table.write_text('\n'.join(rows)+'\n')
    selected=board_config if board_config is not None else {BOARD_FLAGS[0]:'y',BOARD_FLAGS[1]:'n'}
    identity(descriptor(selected))  # Validate before emitting a build defaults file.
    board_values={name:value for name,value in selected.items() if name in BOARD_FLAGS or name=='AMITY_BOARD_RESET_GPIO' or name.startswith('AMITY_TENTACLE')}
    values={**REQUIRED,**board_values,'SECURE_BOOT_SIGNING_KEY':json.dumps(native_reference),'AMITY_RELEASE_PUBLIC_KEY_DER_HEX':json.dumps(key.hex()),'PARTITION_TABLE_CUSTOM_FILENAME':json.dumps(str(table.resolve()))}
    config=out/'sdkconfig.production.defaults'
    config.write_text('# Offline production profile; device provisioning is a separate physical operation.\n'+'\n'.join('CONFIG_'+name+'='+value for name,value in values.items())+'\n')
    evidence=audit(values,partitions(table),key)
    (out/'profile-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    return config,evidence


def storage_keys(partition_file,out):
    # Use the maintained HKDF implementation; the target uses PSA HKDF with
    # these same fixed domain labels. Inputs and outputs are private artifacts.
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    data=Path(partition_file).read_bytes()
    if len(data)!=4096 or zlib.crc32(data[:64],0xffffffff)!=int.from_bytes(data[64:68],'little'):
        raise ValueError('Expected the pinned SDK 4096-byte NVS keys partition with valid CRC')
    out.mkdir(parents=True,exist_ok=True);os.chmod(out,0o700)
    paths=[]
    for name in ('amity_cfg','fctry'):
        key=HKDF(algorithm=hashes.SHA256(),length=64,salt=b'Amity production NVS v1',info=('amity/nvs/partition/'+name).encode()).derive(data[:64])
        value=key+zlib.crc32(key,0xffffffff).to_bytes(4,'little')+b'\xff'*(4096-68)
        target=out/(name+'.nvs-key.bin')
        descriptor=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(descriptor,'wb') as stream:stream.write(value)
        paths.append(target)
    return paths


def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    create=sub.add_parser('prepare');create.add_argument('--publisher-public-key',type=Path,required=True);create.add_argument('--native-signing-key-reference',required=True);create.add_argument('--output-dir',type=Path,required=True);create.add_argument('--board-sdkconfig',type=Path,help='Preserve an explicit validated development GPIO profile; otherwise select the fixed legacy board')
    check=sub.add_parser('audit');check.add_argument('--publisher-public-key',type=Path,required=True);check.add_argument('--sdkconfig',type=Path,required=True);check.add_argument('--partitions',type=Path,required=True);check.add_argument('--output',type=Path)
    derive=sub.add_parser('derive-storage-keys');derive.add_argument('--nvs-key-partition',type=Path,required=True);derive.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args()
    if args.command=='derive-storage-keys':
        for path in storage_keys(args.nvs_key_partition,args.output_dir):print('Private SDK NVS encryption key file:',path)
        return
    key=publisher_der(args.publisher_public_key)
    if args.command=='prepare':
        path,evidence=prepare(key,args.native_signing_key_reference,args.output_dir,read_board_config(args.board_sdkconfig) if args.board_sdkconfig else None);print('Production build defaults:',path)
    else:
        evidence=audit(read_config(args.sdkconfig),partitions(args.partitions),key)
        if args.output:args.output.write_text(json.dumps(evidence,indent=2)+'\n')
        print(json.dumps(evidence,indent=2))
    if not evidence['profile_valid']:raise SystemExit(1)


if __name__=='__main__':main()
