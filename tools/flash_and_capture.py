#!/usr/bin/env python3
"""Flash the built S3 image and retain the first USB boot log privately."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import re
import time
import serial

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',default='/dev/cu.usbmodem31201')
parser.add_argument('--seconds',type=float,default=35)
parser.add_argument('--log',type=Path,default=Path('test-results/boot.log'))
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
os.chdir(root)
args.log.parent.mkdir(parents=True,exist_ok=True)
with open(root/'test-results/flash.log','wb') as log:
    result=subprocess.run(['idf.py','-p',args.port,'flash'],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    raise SystemExit('Flash failed; see test-results/flash.log')
fd=os.open(args.log,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
with os.fdopen(fd,'wb') as log,serial.Serial(args.port,115200,timeout=.25) as port:
    end=time.monotonic()+args.seconds
    while time.monotonic()<end:
        data=port.read(8192)
        if data:log.write(data);log.flush()
text=args.log.read_text(errors='replace')
print('Boot capture saved privately to',args.log)
print('Application ready:', 'Ready. Administration:' in text)
print('Administration: open the HTTPS page to create a password, or use your existing password.')
print('Matter setup captured:', 'AMITY MATTER QR PAYLOAD:' in text)
print('Panic detected:', 'Guru Meditation' in text or 'abort()' in text)
credentials={}
for key,pattern in [('matter_qr_payload',r'AMITY MATTER QR PAYLOAD: (\S+)')]:
    match=re.search(pattern,text)
    if match:credentials[key]=match.group(1)
if credentials:
    path=root/'test-results/setup.json'
    previous=json.loads(path.read_text()) if path.exists() else {}
    previous.update(credentials)
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
    with os.fdopen(fd,'w') as output:json.dump(previous,output,indent=2)
if 'Ready. Administration:' not in text or 'Guru Meditation' in text or 'abort()' in text:
    raise SystemExit(1)
