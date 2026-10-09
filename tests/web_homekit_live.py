#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Check the live HomeKit QR, polling and real five-minute pairing timeout."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path
from playwright.sync_api import sync_playwright, expect
from cec_web_activity import Admin,write_private

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True);parser.add_argument('--credentials',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--wait-expiry',action='store_true')
    args=parser.parse_args();os.umask(0o077)
    password=json.loads(args.credentials.read_text())['admin_password'];a=Admin(args.url);a.login(password)
    initial={k:a.request('/api/'+k) for k in ['homekit','matter','config']};assert not initial['homekit']['paired']
    checks=[]
    def passed(name):checks.append(name);print('PASS',name,flush=True)
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True)
            try:
                page=browser.new_page(ignore_https_errors=True);page.set_default_timeout(45000)
                errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
                page.goto(args.url);page.locator('#password').fill(password);page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible();page.locator('[data-tab=homekit]').click()
                expect(page.locator('#toggleHomekit')).to_be_enabled()
                expect(page.locator('#homekitQr')).to_be_hidden();expect(page.locator('#homekitPairingCode')).to_have_text('')
                opened_at=time.monotonic();page.locator('#toggleHomekit').click()
                expect(page.locator('#homekitQr svg')).to_be_visible()
                pairing=a.request('/api/homekit');assert pairing['pairing']
                expect(page.locator('#homekitPairingCode')).to_have_text('Pairing code: '+pairing['manual_code'])
                passed('live separate tab starts pairing and displays matching QR and PIN')
                image=args.output.with_suffix('.qr.png');expected=args.output.with_suffix('.payload.json')
                write_private(expected,{'qr_payload':pairing['qr_payload']});page.locator('#homekitQr').screenshot(path=str(image))
                subprocess.run(['swift','-module-cache-path',str((args.output.parent/'homekit-swift-module-cache').resolve()),
                    str(Path(__file__).with_name('decode_matter_qr.swift')),str(image),str(expected)],check=True,stdout=subprocess.DEVNULL)
                passed('rendered HomeKit QR independently decodes to the device setup payload')
                if args.wait_expiry:
                    deadline=time.monotonic()+325;samples=[]
                    while time.monotonic()<deadline:
                        page.wait_for_timeout(5000)
                        state=a.request('/api/homekit');s=a.request('/api/status')
                        samples.append({k:s.get(k) for k in ['free_internal_heap','largest_internal_block','free_heap']})
                        if not state['pairing']:
                            assert 'manual_code' not in state and 'qr_payload' not in state
                            expect(page.locator('#homekitQr')).to_be_hidden()
                            expect(page.locator('#homekitPairingCode')).to_have_text('')
                            expect(page.locator('#toggleHomekit')).to_have_text('Start pairing')
                            assert time.monotonic()-opened_at>=295,'Pairing closed before its five-minute window'
                            passed('actual five-minute timeout clears backend and live browser setup material')
                            write_private(args.output.with_suffix('.memory.json'),samples);break
                    else:raise AssertionError('HomeKit pairing did not expire')
                else:
                    page.locator('#toggleHomekit').click();expect(page.locator('#homekitQr')).to_be_hidden()
                assert not errors,errors;passed('no live browser script errors')
            finally:browser.close()
        assert a.request('/api/config')==initial['config'] and a.request('/api/matter')['fabrics']==initial['matter']['fabrics']
        passed('HomeKit pairing UI preserves saved activities and Matter fabrics')
        write_private(args.output,{'passed':checks})
    finally:a.request('/api/homekit/close',{})

if __name__=='__main__':main()
