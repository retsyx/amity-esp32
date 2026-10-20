#!/usr/bin/env python3
"""Install an already built image through authenticated OTA; require that exact version ready."""
import argparse
import json
import hashlib
import http.client
from urllib.parse import urlsplit
import os
from pathlib import Path
import threading
import subprocess
import sys
import time
import urllib.request
import uuid
from cec_web_activity import Admin, read_password


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True);parser.add_argument('--credentials',required=True,type=Path)
    parser.add_argument('--manifest',type=Path);parser.add_argument('--firmware',required=True,type=Path);parser.add_argument('--serial');parser.add_argument('--cohort',action='store_true',help='Exercise the managed coordinator-last chunk path for the local DUT')
    parser.add_argument('--output',required=True,type=Path);args=parser.parse_args()
    image=args.firmware.read_bytes();version=image[48:80].split(b'\0')[0].decode('ascii')
    elf=args.firmware.with_suffix('.elf')
    if elf.exists():
        if hashlib.sha256(elf.read_bytes()).digest()!=image[176:208]:raise AssertionError('Binary and ELF do not belong to the same build')
        subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'tools/check_firmware_layouts.py'),'--elf',str(elf)],check=True)
    stop=threading.Event();port=None;capture=None;worker=None
    if args.serial:
        import serial
        port=serial.Serial(port=None,baudrate=115200,timeout=.2)
        port.dtr=False;port.rts=False;port.port=args.serial;port.open()
        capture=args.output.with_suffix('.serial.log').open('wb');os.chmod(capture.name,0o600)
        def read_serial():
            while not stop.is_set():
                try:
                    data=port.read(8192)
                    if data:capture.write(data);capture.flush()
                except OSError:return
        worker=threading.Thread(target=read_serial,daemon=True);worker.start()
        time.sleep(10)
    admin=Admin(args.url);password=read_password(args.credentials)
    try:
        admin.login(password);old=admin.request('/api/setup')
        # Older bridge firmware predates manifests; every newer upload binds the
        # exact artifact and its compatibility contract before writing a slot.
        manifest=args.manifest or args.firmware.with_suffix('.release.json')
        if not args.cohort and manifest.exists() and (old.get('update_manifest_required') or tuple(int(v) for v in old['version'].split('.')[:3])>=(0,1,55)):
            proof=manifest.with_suffix('.sig')
            admin.request('/api/v2/updates/validate',dict(request_id=uuid.uuid4().hex,manifest=manifest.read_text(),signature=proof.read_bytes().hex() if proof.exists() else ''))
        if args.cohort:
            system=admin.request('/api/v2/system');owner=system['octopus_id'];proof=manifest.with_suffix('.sig')
            admin.request('/api/v2/updates/cohort',dict(request_id=uuid.uuid4().hex,manifest=manifest.read_text(),signature=proof.read_bytes().hex() if proof.exists() else '',targets=[owner]))
            job=admin.request('/api/v2/updates/cohort');job_id=job['job_id']
            endpoint=urlsplit(args.url);connection=http.client.HTTPSConnection(endpoint.hostname,endpoint.port or 443,context=admin.context,timeout=90)
            try:
                for offset in range(0,len(image),2048):
                    body=json.dumps(dict(request_id=uuid.uuid4().hex,cohort_job_id=job_id,target_octopus_id=owner,offset=str(offset),data_hex=image[offset:offset+2048].hex()),separators=(',',':'))
                    connection.request('POST','/api/v2/updates/chunk',body,{'Authorization':'Bearer '+admin.token,'Content-Type':'application/json'})
                    response=connection.getresponse();value=json.loads(response.read())
                    if response.status!=200:raise AssertionError('Managed update checkpoint: '+str(value.get('error',response.status)))
                    if offset%(2048*128)==0:print('Managed checkpoint',offset,'/',len(image),flush=True)
            finally:connection.close()
            job=admin.request('/api/v2/updates/cohort')
            if job['targets'][0]['offset']!=str(len(image)):raise AssertionError('Coordinator checkpoint differs from exact image length')
            admin.request('/api/v2/updates/install',dict(request_id=uuid.uuid4().hex,cohort_job_id=job_id,target_octopus_id=owner))
        else:
            request=urllib.request.Request(args.url.rstrip('/')+'/api/firmware',data=image,headers={'Authorization':'Bearer '+admin.token,'Content-Type':'application/octet-stream'})
            with urllib.request.urlopen(request,context=admin.context,timeout=300) as response:json.load(response)
        print('OTA admitted; waiting for',version,flush=True)
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            time.sleep(2)
            try:
                new=admin.request('/api/setup')
                if new['boot_id']!=old['boot_id'] and new['ready']:
                    if new['version']!=version:raise AssertionError('Firmware rolled back to '+new['version'])
                    if new.get('image_elf_sha256')!=image[176:208].hex():raise AssertionError('Ready image does not match the uploaded ELF identity')
                    admin.login(password);snapshot=admin.request('/api/v2/system')
                    result=dict(firmware=version,ready=True,system=snapshot,image_sha256=hashlib.sha256(image).hexdigest(),image_elf_sha256=image[176:208].hex())
                    if args.cohort:
                        for _ in range(90):
                            cohort=admin.request('/api/v2/updates/cohort')
                            if cohort['phase']=='complete':break
                            if cohort['phase'] in ('failed','rolled_back','recovery_required'):raise AssertionError('Managed cohort did not reconcile: '+cohort['phase'])
                            time.sleep(1)
                        if cohort['phase']!='complete':raise AssertionError('Managed cohort never reached exact healthy completion')
                        result['cohort']=cohort
                    args.output.write_text(json.dumps(result,indent=2)+'\n');args.output.chmod(0o600)
                    admin.request('/api/logout',{});print('Ready',version,flush=True);return
            except OSError:pass
        raise AssertionError('New firmware did not become ready within 180 seconds')
    finally:
        stop.set()
        if worker:worker.join(2)
        if port:port.close()
        if capture:capture.close()


if __name__=='__main__':main()
