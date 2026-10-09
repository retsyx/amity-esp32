#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise secure Go password prompting with a real pseudo-terminal (Unix hosts)."""
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tempfile
import termios
import time
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=ROOT/'test-results') as temporary:
    for mode in ('match','mismatch','interrupt'):
        master,slave=pty.openpty()
        proc=subprocess.Popen([str(ROOT/'build/amity-provision-test'),'--prompt-password','--out',str(Path(temporary)/mode),str(ROOT/'build/amity-firmware.zip')],stdin=slave,stdout=slave,stderr=slave)
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
                assert time.monotonic()<deadline
                time.sleep(.01)
            if mode=='interrupt':
                proc.send_signal(signal.SIGINT);assert proc.wait(timeout=5)==130
                assert termios.tcgetattr(slave)[3]&termios.ECHO
            else:
                os.write(master,b'terminal secret password\n');wait_for(b'Confirm admin password: ')
                deadline=time.monotonic()+2
                while termios.tcgetattr(slave)[3]&termios.ECHO:
                    assert time.monotonic()<deadline
                    time.sleep(.01)
                os.write(master,b'terminal secret password\n' if mode=='match' else b'other password\n')
                assert proc.wait(timeout=10)==(0 if mode=='match' else 1)
                while select.select([master],[],[],.1)[0]:received+=os.read(master,4096)
                assert b'terminal secret password' not in received
                assert termios.tcgetattr(slave)[3]&termios.ECHO
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
