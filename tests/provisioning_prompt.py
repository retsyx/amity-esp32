#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise secure Go password prompting with a real pseudo-terminal (Unix hosts).

Usage: provisioning_prompt.py <amity-provision binary>
"""
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zipfile
TOOL=sys.argv[1]

def write_package(path):
    """The smallest firmware package loadBundle accepts, like fixture() in bundle_test.go."""
    partitions=[('esp_secure_cert',0x3f,6,0xd000,0x2000,1),('amity_factory',1,0x40,0xf000,0x1000,0),
                ('nvs',1,2,0x10000,0x10000,0),('nvs_keys',1,4,0x20000,0x1000,1),('otadata',1,0,0x21000,0x2000,0),
                ('phy_init',1,1,0x23000,0x1000,0),('amity_cfg',1,2,0x24000,0x1c000,0),
                ('ota_0',0,0x10,0x40000,0x3d0000,0),('ota_1',0,0x11,0x410000,0x3d0000,0)]
    table=b''.join(struct.pack('<HBBII16sI',0x50aa,typ,sub,off,size,name.encode(),flags)
                   for name,typ,sub,off,size,flags in partitions)
    table+=b'\xeb\xeb'+b'\xff'*14+hashlib.md5(table).digest()
    app=bytes([0xe9])+bytes(11)+bytes([9])+bytes(11)
    files={'bootloader.bin':app,'partition-table.bin':table,'ota-data.bin':b'\xff'*8192,'amity.bin':app}
    offsets={'bootloader.bin':0,'partition-table.bin':0xc000,'ota-data.bin':0x21000,'amity.bin':0x40000}
    manifest={'format':'amity-firmware','version':3,'firmware_version':'test','chip':'esp32s3',
              'flash_size':0x800000,'factory_offset':0xf000,'factory_size':4096,'vendor_id':0xfff1,'product_id':0x8000,
              'images':[{'path':p,'offset':o,'sha256':hashlib.sha256(files[p]).hexdigest()} for p,o in offsets.items()]}
    with zipfile.ZipFile(path,'w') as z:
        for name,data in files.items():z.writestr(name,data)
        z.writestr('manifest.json',json.dumps(manifest))

with tempfile.TemporaryDirectory(prefix='amity-provisioning-prompt-') as temporary:
    package=Path(temporary)/'amity-firmware.zip'
    write_package(package)
    for mode in ('match','mismatch','interrupt'):
        master,slave=pty.openpty()
        proc=subprocess.Popen([TOOL,'--prompt-password','--out',str(Path(temporary)/mode),str(package)],stdin=slave,stdout=slave,stderr=slave)
        received=b''
        def wait_for(text):
            global received
            deadline=time.monotonic()+15
            while text not in received:
                assert time.monotonic()<deadline,'terminal prompt timed out'
                if select.select([master],[],[],.1)[0]:received+=os.read(master,4096)
        try:
            wait_for(b'Initial admin password: ')
            deadline=time.monotonic()+2
            while termios.tcgetattr(slave)[3]&termios.ECHO:
                assert time.monotonic()<deadline,'echo stayed on at the first prompt'
                time.sleep(.01)
            if mode=='interrupt':
                proc.send_signal(signal.SIGINT);assert proc.wait(timeout=5)==130
                assert termios.tcgetattr(slave)[3]&termios.ECHO,'interrupt left echo off'
            else:
                os.write(master,b'terminal secret password\n');wait_for(b'Confirm admin password: ')
                deadline=time.monotonic()+2
                while termios.tcgetattr(slave)[3]&termios.ECHO:
                    assert time.monotonic()<deadline,'echo stayed on at the confirmation prompt'
                    time.sleep(.01)
                os.write(master,b'terminal secret password\n' if mode=='match' else b'other password\n')
                assert proc.wait(timeout=10)==(0 if mode=='match' else 1)
                while select.select([master],[],[],.1)[0]:received+=os.read(master,4096)
                assert b'terminal secret password' not in received,'the password reached the terminal'
                assert termios.tcgetattr(slave)[3]&termios.ECHO,'the prompt left echo off'
            assert (Path(temporary)/mode).exists()==(mode=='match')
            if mode=='match':
                pairing=(Path(temporary)/mode/'amity-pairing.txt').read_text()
                assert 'terminal secret password' not in pairing
                assert 'Initial admin password:' not in pairing
                assert not (Path(temporary)/mode/'amity-admin-password.txt').exists()
                assert 'Hostname: amity\n' in pairing
                assert (Path(temporary)/mode/'amity-pairing.png').is_file()
            print('PASS secure terminal prompt:',mode)
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            os.close(master);os.close(slave)
