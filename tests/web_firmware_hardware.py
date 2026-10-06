#!/usr/bin/env python3
"""Verify OTA version guards and the browser flow on the authorized DUT.

Header-only probes cannot install an image. Finish by reinstalling the supplied
current release through the browser, restoring a valid image in the OTA slot.
"""
import argparse
import hashlib
import json
import re
import ssl
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse
from pathlib import Path
from playwright.sync_api import sync_playwright, expect
from cec_web_activity import Admin, write_private

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True);parser.add_argument('--credentials',type=Path,required=True)
    parser.add_argument('--firmware',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--skip-probes',action='store_true',help='Run only the browser OTA and restart flow')
    args=parser.parse_args();a=Admin(args.url)
    password=json.loads(args.credentials.read_text())['admin_password'];a.login(password)
    binary=args.firmware.read_bytes();version=binary[48:80].split(b'\0')[0].decode();checks=[]
    before={k:a.request('/api/'+k) for k in ['status','config','matter','remotes']}
    assert before['status']['version']==version
    host=urlparse(args.url).hostname
    certificate=hashlib.sha256(ssl.get_server_certificate((host,443)).encode()).hexdigest()
    def probe(candidate,confirmation=None,wrong_project=False):
        data=bytearray(binary[:288]);data[48:80]=candidate.encode().ljust(32,b'\0')
        if wrong_project:data[80:112]=b'other-project'.ljust(32,b'\0')
        headers={'Authorization':'Bearer '+a.token,'Content-Type':'application/octet-stream'}
        if confirmation is not None:headers['X-Amity-Confirm-Version']=confirmation
        req=urllib.request.Request(args.url+'/api/firmware',data=data,headers=headers)
        try:
            with urllib.request.urlopen(req,context=a.context,timeout=30) as response:return response.status,response.read().decode()
        except urllib.error.HTTPError as error:return error.code,error.read().decode()
    if not args.skip_probes:
        for candidate,approval in [('0.0.0',None),('0.0.0','stale-version'),('development',None)]:
            status,body=probe(candidate,approval);assert status==409,(status,body)
        checks.append('unapproved downgrade, stale approval and unknown version rejected with HTTP 409')
        for candidate,approval in [('0.0.0',version),('development',version),(version,None),('999.0.0',None)]:
            status,body=probe(candidate,approval);assert status==400 and 'Firmware validation failed' in body,(status,body)
        checks.append('confirmed older/unknown and ordinary equal/newer versions reach full image validation')
        status,body=probe('0.0.0',version,True);assert status==400 and 'not for this Amity target' in body
        checks.append('explicit version approval cannot bypass target validation')
    assert a.request('/api/status')['version']==version
    boot_before=a.request('/api/setup')['boot_id']
    with sync_playwright() as p:
        browser=p.chromium.launch(headless=True);page=browser.new_page(ignore_https_errors=True);errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
        page.goto(args.url);page.locator('#password').fill(password);page.locator('#password').press('Enter');expect(page.locator('#admin')).to_be_visible(timeout=30000);page.locator('[data-tab=advanced]').click()
        assets=[];page.on('response',lambda response: assets.append(response) if urlparse(response.url).path in ['/', '/app.js', '/style.css'] else None)
        uploads=[];page.on('request',lambda r:uploads.append(r) if r.url.endswith('/api/firmware') else None)
        def cancel(dialog):
            assert 'Downgrading is not supported. Continue at your own risk?' in dialog.message
            assert version in dialog.message;dialog.dismiss()
        data=bytearray(binary[:288]);data[48:80]=b'0.0.0'.ljust(32,b'\0')
        page.once('dialog',cancel);page.locator('#firmware').set_input_files({'name':'older.bin','mimeType':'application/octet-stream','buffer':bytes(data)})
        expect(page.locator('#firmware')).to_be_enabled();expect(page.locator('#firmware')).to_have_value('');assert not uploads
        checks.append('live browser shows downgrade warning; cancelling sends no upload')
        asset_count=len(assets)
        def accept(dialog):assert 'Install firmware '+version in dialog.message;dialog.accept()
        page.once('dialog',accept);page.locator('#firmware').set_input_files(str(args.firmware.resolve()))
        expect(page.locator('#firmwareUpdate')).to_be_visible(timeout=30000)
        expect(page.locator('#login')).to_be_hidden()
        expect(page.locator('#uploadProgress')).to_be_visible()
        checks.append('live upload displays progress and hides login')
        expect(page.locator('#firmwareUpdateStatus')).to_have_text(re.compile(r'Firmware uploaded\..*|Waiting for Amity.*|Amity is starting.*'),timeout=120000)
        expect(page.locator('#login')).to_be_hidden()
        expect(page.locator('#uploadProgress')).to_be_visible()
        checks.append('live reboot waits for the new device boot without enabling password entry')
        expect(page.locator('#message')).to_have_text('Firmware update complete. Amity is running version '+version+'. Please log in.',timeout=180000)
        expect(page.locator('#login')).to_be_visible()
        checks.append('live browser confirms the installed version and displays completion on login')
        fresh_assets=assets[asset_count:]
        expected={'/':'index.html','/app.js':'app.js','/style.css':'style.css'}
        for path,name in expected.items():
            responses=[r for r in fresh_assets if urlparse(r.url).path==path]
            assert responses and responses[-1].status==200,path
            assert responses[-1].headers['cache-control']=='no-cache'
            assert hashlib.sha256(responses[-1].body()).digest()==hashlib.sha256((Path(__file__).resolve().parents[1]/'components/web_admin/assets'/name).read_bytes()).digest()
        checks.append('completion fetches fresh HTML, JavaScript and CSS matching the new build')
        page.locator('#password').fill(password);page.locator('#password').press('Enter')
        expect(page.locator('#admin')).to_be_visible(timeout=30000)
        checks.append('password login succeeds after the UI reports readiness')
        assert len(uploads)==1 and 'x-amity-confirm-version' not in uploads[0].headers
        assert not errors,errors;browser.close()
    for _ in range(50):
        time.sleep(2)
        try:
            a.login(password);state=a.request('/api/status')
            if state['ready'] and state['version']==version:break
        except Exception:pass
    else:raise AssertionError('Device did not return after browser OTA')
    health=a.request('/api/setup');assert health['ready'] and health['version']==version and health['boot_id']!=boot_before
    checks.append('public boot ID changes and reports healthy firmware only after restart')
    if state['current_activity']!=before['status']['current_activity']:a.activity(before['status']['current_activity'])
    assert a.request('/api/config')==before['config'];assert a.request('/api/matter')['fabrics']==before['matter']['fabrics'];assert a.request('/api/remotes')['bonded']==before['remotes']['bonded']
    assert state['hostname']==before['status']['hostname'];assert hashlib.sha256(ssl.get_server_certificate((host,443)).encode()).hexdigest()==certificate
    checks.append('live browser raw binary OTA succeeds and preserves password, hostname, TLS, configuration and pairings')
    write_private(args.output,{'version':version,'passed':checks})
    for check in checks:print('PASS',check)
if __name__=='__main__':main()
