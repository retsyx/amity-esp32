#!/usr/bin/env python3
"""Validate production preparation and rejection paths without touching hardware."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import zlib
import os

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('production_profile',ROOT/'tools/production_profile.py');profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);key=path/'native-independent-test.pem';public=path/'publisher.pem'
    subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',str(key)],check=True,capture_output=True)
    subprocess.run(['openssl','pkey','-in',str(key),'-pubout','-out',str(public)],check=True,capture_output=True)
    der=profile.publisher_der(public)
    config,evidence=profile.prepare(der,'/offline-separate-native-rsa3072.pem',path/'prepared')
    assert evidence['profile_valid'] and not evidence['production_qualified'] and not evidence['device_mutations']
    table=profile.partitions(config.parent/'partitions.production.csv');values=profile.read_config(config)
    assert table['amity_factory']['encrypted'] and not table['amity_cfg']['encrypted']
    for name in profile.REQUIRED:
        changed=dict(values);changed[name]='n';assert not profile.audit(changed,table,der)['profile_valid'],name
    changed=dict(values);changed['AMITY_RELEASE_PUBLIC_KEY_DER_HEX']='"00"';assert not profile.audit(changed,table,der)['profile_valid']
    assert not profile.audit(values,profile.partitions(ROOT/'partitions.csv'),der)['profile_valid']
    changed=dict(values);changed.pop('AMITY_BOARD_LEGACY_1T');assert not profile.audit(changed,table,der)['profile_valid']
    changed=dict(values);changed['AMITY_BOARD_DEVELOPMENT_4T']='y';assert not profile.audit(changed,table,der)['profile_valid']
    board={'AMITY_BOARD_LEGACY_1T':'n','AMITY_BOARD_DEVELOPMENT_4T':'y','AMITY_BOARD_RESET_GPIO':'-1'}
    for slot in range(4):
        for name in ('FRONT_GPIO','BACK_GPIO'):board[f'AMITY_TENTACLE{slot}_{name}']='-1'
    development,evidence=profile.prepare(der,'/offline-separate-native-rsa3072.pem',path/'development',board)
    assert evidence['profile_valid'] and evidence['hardware_profile']=='esp32s3-development-4t-v1' and len(evidence['hardware_mapping_sha256'])==64
    assert not evidence['production_qualified'] and not evidence['device_mutations']
    invalid=profile.read_config(development);invalid['AMITY_BOARD_RESET_GPIO']='19';assert not profile.audit(invalid,table,der)['profile_valid']
    changed=dict(values);changed['SECURE_BOOT_ALLOW_JTAG']='y';assert not profile.audit(changed,table,der)['profile_valid']
    subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-384','-out',str(key)],check=True,capture_output=True)
    subprocess.run(['openssl','pkey','-in',str(key),'-pubout','-out',str(public)],check=True,capture_output=True)
    try:profile.publisher_der(public)
    except ValueError:pass
    else:raise AssertionError('Unsupported publisher curve accepted')
    root=bytes(range(64));source=path/'root-nvs.bin';source.write_bytes(root+zlib.crc32(root,0xffffffff).to_bytes(4,'little')+b'\xff'*(4096-68))
    try:
        files=profile.storage_keys(source,path/'private-keys')
        expected=('d18780a0fe11189ccfcb13b418ac3801db33224df7aa339024bd03b8c07199d089606d9e434526e9c2ad8e5820e7bf2fbc3d5d2733b509c38314620687ddc76b','e1b5a4d51e255acdb0be4899a6edbe9bf958094a47aa4262eb42a272590f5eecb19b1aef0a36fd157ab223d7c3e44f5ee9d663653c8b705096c8749d1fa6152e')
        for target,vector in zip(files,expected):
            data=target.read_bytes();assert data[:64].hex()==vector and len(data)==4096 and zlib.crc32(data[:64],0xffffffff)==int.from_bytes(data[64:68],'little')
            assert target.stat().st_mode&0o777==0o600
        assert files[0].read_bytes()!=files[1].read_bytes()
        source.write_bytes(b'\x00'*4096)
        try:profile.storage_keys(source,path/'bad')
        except ValueError:pass
        else:raise AssertionError('Corrupt root accepted')
    except ModuleNotFoundError as error:
        raise RuntimeError('Run production key vector checks in the pinned SDK Python environment with cryptography') from error
print('PASS offline production profile enforces independent publisher root, native security, protected NVS and unchanged partition geometry; no device mutations')
