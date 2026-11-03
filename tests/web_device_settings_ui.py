#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise shipped per-device settings, hostname and reset UI against a local fixture.

The fixture qualifies browser requests and presentation, not physical HDMI address discovery, an actual restart or multiple-device hardware.
"""
from copy import deepcopy
import json
import os
import tempfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.parse import urlsplit
from playwright.sync_api import sync_playwright, expect

ROOT=Path(__file__).resolve().parents[1]
OUTPUT=Path(os.environ.get('AMITY_UI_TEST_OUTPUT') or tempfile.mkdtemp(prefix='amity-settings-ui-'))
OUTPUT.mkdir(parents=True,exist_ok=True)
MAIN,MEMBER=(f'{n:032x}' for n in (1,2))
BOOT,REMOTE_BOOT,RENAMED_BOOT='a'*32,'b'*32,'c'*32
fixture=dict(hostname='amity-dut',boot_id=BOOT,hostname_probes=0,hostname_ready=False,early_navigation=False,determined=None,calls=[],gets=[],fail=False,
             settings_fail=False,settings_outcome='applied',settings_http500=False,remote_status='ready',named=True,operations={},next_id=100)
editor=dict(version=1,activities=[],device_choices=[],expected_config_revision='1',expected_presentation_revision='1')
configuration=dict(version=3,controller_octopus_id=MAIN,config_revision='1',presentation_revision='1',activities=[],zones=[],
                   octopuses=[dict(octopus_id=MAIN,supported_tentacles=1),dict(octopus_id=MEMBER,supported_tentacles=4)])
def adapter(slot,configured=True,default='1.0.0.0',observed=None):
    return dict(tentacle_id=slot,name=f'HDMI adapter {slot+1}',configured=configured,
                default_front_physical_address=default,observed_front_physical_address=observed,
                effective_front_physical_address=(observed or default) if configured else None)
settings={MAIN:dict(boot_id=BOOT,settings_revision='1',adapters=[adapter(0)]),
          MEMBER:dict(boot_id=REMOTE_BOOT,settings_revision='9007199254740993',adapters=[
              adapter(0,default='2.0.0.0',observed='3.0.0.0'),adapter(1,default='4.0.0.0'),
              adapter(2,configured=False),adapter(3,default='A.B.0.0')])}
class Handler(SimpleHTTPRequestHandler):
    def __init__(self,*args,**kwargs):super().__init__(*args,directory=str(ROOT/'components/web_admin/assets'),**kwargs)
    def log_message(self,*args):pass
    def reply(self,data,status=200,headers=None):
        data=json.dumps(data).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)))
        for name,value in (headers or {}).items():self.send_header(name,value)
        self.end_headers();self.wfile.write(data)
    def do_GET(self):
        fixture['gets'].append(self.path)
        if self.path=='/api/ready':return self.reply(dict(device_id=MAIN,boot_id=fixture['boot_id'],hostname=fixture['hostname'],ready=True),headers={'Access-Control-Allow-Origin':'*','Cache-Control':'no-store'})
        status=dict(version='0.1.73',hostname=fixture['hostname'],front_physical_address='1.0.0.0',determined_front_physical_address=fixture['determined'],
                    hardware='Amity',ready=True,current_activity=-1,front_address=4,back_address=0,devices=[],device_choices=[],expected_boot_id=fixture['boot_id'],expected_state_revision='1')
        system=dict(octopus_id=MAIN,hostname=fixture['hostname'],named_devices=[dict(octopus_id=MAIN,hostname=fixture['hostname'])],
                    membership=dict(peers=[dict(octopus_id=MEMBER,enrolled=True,ready=fixture['remote_status']=='ready')]))
        if fixture['named']:system['named_devices'].append(dict(octopus_id=MEMBER,hostname='amity-bedroom'))
        values={'/api/setup':dict(required=False,hostname=fixture['hostname']),'/api/activity-editor':editor,
                '/api/activity-editor/status':status,'/api/config':configuration,'/api/system':system,
                '/api/remotes':dict(bonded=False,ready=True,devices=[]),'/api/matter':dict(fabrics=[]),'/api/homekit':dict(available=False)}
        if self.path in values:return self.reply(deepcopy(values[self.path]))
        if self.path.startswith('/api/devices/') and self.path.endswith('/settings'):
            owner=self.path.removeprefix('/api/devices/').split('/',1)[0];state='ready' if owner==MAIN else fixture['remote_status']
            return self.reply(dict(status=state,settings=deepcopy(settings[owner]) if state!='unsupported' else None,error=''))
        if self.path.startswith('/api/operations/'):
            operation=fixture['operations'][self.path.rsplit('/',1)[-1]];operation['polls']+=1
            stage='admitted' if operation['polls']==1 else operation['outcome']
            return self.reply(dict(operation_id=operation['id'],stage=stage,rebooting=False))
        super().do_GET()
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))))
        if self.path=='/api/login':return self.reply(dict(token='fixture-session'))
        fixture['calls'].append((self.path,deepcopy(data)))
        if fixture['fail']:return self.reply(dict(error='Save failed'),500)
        if self.path=='/api/hostname':
            previous=fixture['boot_id'];fixture['hostname']=data['hostname'].lower();fixture['boot_id']=RENAMED_BOOT;settings[MAIN]['boot_id']=RENAMED_BOOT
            fixture['hostname_probes']=0;fixture['hostname_ready']=fixture['early_navigation']=False
            return self.reply(dict(hostname=fixture['hostname'],rebooting=True,previous_boot_id=previous,device_id=MAIN))
        if self.path.startswith('/api/devices/') and self.path.endswith('/settings'):
            owner=self.path.removeprefix('/api/devices/').split('/',1)[0];current=settings[owner]
            if fixture['settings_fail']:return self.reply(dict(stage='failed',message='Stop activities before saving.',rebooting=False))
            if data['expected_boot_id']!=current['boot_id'] or data['expected_settings_revision']!=current['settings_revision']:
                return self.reply(dict(stage='failed',message='Settings changed. Refresh before saving.',rebooting=False))
            assert set(data)=={'request_id','expected_boot_id','expected_settings_revision','changes'} and len(data['request_id'])==32
            seen=set()
            for change in data['changes']:
                row=current['adapters'][change['tentacle_id']]
                assert change['tentacle_id'] not in seen;seen.add(change['tentacle_id'])
                if not row['configured']:
                    return self.reply(dict(stage='failed',message='HDMI adapter is not configured. Refresh before saving.',rebooting=False))
            for change in data['changes']:
                row=current['adapters'][change['tentacle_id']]
                row['default_front_physical_address']=change['default_front_physical_address'] or '1.0.0.0'
                row['effective_front_physical_address']=row['observed_front_physical_address'] or row['default_front_physical_address']
            current['settings_revision']=str(int(current['settings_revision'])+1)
            if fixture['settings_http500']:return self.reply(dict(error='Connection closed after saving'),500)
            operation_id=f'{fixture["next_id"]:032x}';fixture['next_id']+=1
            fixture['operations'][operation_id]=dict(id=operation_id,polls=0,owner=owner,outcome=fixture['settings_outcome'])
            return self.reply(dict(operation_id=operation_id,stage='admitted',rebooting=False))
        if self.path=='/api/factory-reset':return self.reply(dict(ok=True))
        return self.reply(dict(error='Unexpected mutation'),404)
def main():
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler);Thread(target=server.serve_forever,daemon=True).start()
    checks=[]
    def passed(label):checks.append(label);print('PASS',label,flush=True)
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True);page=browser.new_page(viewport=dict(width=430,height=900));errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
            def renamed_origin(route):
                # Interception qualifies browser CORS/readiness/navigation logic,
                # not an actual mDNS lookup, restart or TLS certificate approval.
                request=route.request;path=urlsplit(request.url).path
                if path=='/api/ready':
                    assert request.method=='GET' and 'authorization' not in request.headers and 'cookie' not in request.headers
                    fixture['hostname_probes']+=1;probe=fixture['hostname_probes']
                    if probe==1:return route.abort('namenotresolved')
                    health=dict(device_id=MAIN,boot_id=RENAMED_BOOT,hostname='living-room',ready=True)
                    if probe==2:health['boot_id']=BOOT
                    elif probe==3:health['hostname']='different-amity'
                    elif probe==4:health['ready']=False
                    else:fixture['hostname_ready']=True
                    return route.fulfill(status=200,content_type='application/json',headers={'Access-Control-Allow-Origin':'*','Cache-Control':'no-store'},body=json.dumps(health))
                if path=='/':
                    fixture['early_navigation']=not fixture['hostname_ready']
                    return route.fulfill(status=200,content_type='text/html',body='<title>living-room</title><h1>living-room</h1>')
                return route.abort('blockedbyclient')
            page.route('https://living-room.local/**',renamed_origin)
            def enter():
                page.goto(f'http://127.0.0.1:{server.server_port}/');page.locator('#password').fill('test');page.locator('#password').press('Enter');expect(page.locator('#admin')).to_be_visible();page.locator('[data-tab=advanced]').click()
            def manage():
                page.locator('[data-tab=amityDevices]').click();expect(page.locator('#managedAmityList > article')).to_have_count(2)
            def open_settings(index=1):
                manage();page.locator('#managedAmityList > article').nth(index).get_by_role('button',name='Settings',exact=True).click()
                d=page.locator('dialog.adapter-settings:visible');expect(d.get_by_role('table')).to_have_count(1);expect(d.locator('tbody tr')).to_have_count(1 if index==0 else 4);return d
            def close(d):d.get_by_role('button',name='Close',exact=True).click();expect(d).to_have_count(0)
            def input_for(d,slot):return d.get_by_role('textbox',name=f'Default address for adapter {slot+1}',exact=True)
            def settings_posts():return [(path,body) for path,body in fixture['calls'] if path.endswith('/settings')]
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            expect(page.locator('h1')).to_have_text('amity-dut');expect(page).to_have_title('amity-dut');expect(page.locator('header .byline')).to_have_count(0);expect(page.locator('footer')).to_have_text('Amity - HDMI-CEC that works!');passed('hostname header before login; footer replaces byline')
            enter();expect(page.locator('#hostname')).to_have_value('amity-dut')
            page.locator('#hostname').fill('Living-Room');page.wait_for_function('() => !polling');page.evaluate('() => refresh()');expect(page.locator('#hostname')).to_have_value('Living-Room');passed('polling preserves edited hostname')
            fixture['fail']=True;page.locator('#changeHostname button').click();expect(page.locator('#message')).to_have_text('Save failed');expect(page.locator('#admin')).to_be_visible();passed('failed hostname save stays editable')
            fixture['fail']=False;page.locator('#changeHostname button').click();expect(page.locator('#hostnameLink')).to_be_visible();expect(page.locator('#hostnameLink')).to_have_attribute('href','https://living-room.local');expect(page.locator('h1')).to_have_text('living-room');expect(page.locator('#admin')).to_be_hidden()
            expect(page).to_have_url('https://living-room.local/',timeout=20000)
            assert fixture['hostname_probes']>=5 and fixture['hostname_ready'] and not fixture['early_navigation']
            passed('hostname change clears the old session, retries DNS and rejects old-boot/wrong-host/not-ready probes before automatically opening the new origin')
            enter();expect(page.locator('#frontPhysicalAddress')).to_have_count(0);expect(page.locator('#changeFrontPhysicalAddress')).to_have_count(0)
            expect(page.locator('#diagnostics')).to_contain_text('Firmware 0.1.73');expect(page.locator('#diagnostics')).to_contain_text('Remote: Disconnected')
            expect(page.locator('#advanced')).not_to_contain_text('Front physical address');passed('global address control moved out of Advanced; firmware and remote diagnostics retained')
            before=deepcopy(fixture['calls']);d=open_settings(0);expect(d.locator('h3')).to_have_text('living-room settings');expect(input_for(d,0)).to_be_enabled()
            expect(d.get_by_role('columnheader')).to_have_text(['Adapter','Default','Detected Address','Selected Address'])
            expect(d.get_by_role('rowheader')).to_have_text(['1']);expect(d.locator('tbody td').nth(1)).to_have_text('—');expect(d.locator('tbody td').nth(2)).to_have_text('1.0.0.0');assert fixture['calls']==before
            expect(d).not_to_contain_text('front');expect(d).not_to_contain_text('Configured HDMI adapters can be edited.');expect(d).not_to_contain_text('restart')
            passed('fixed adapter remains editable; opening named device settings sends no mutation or scan');close(d)
            d=open_settings();expect(d.locator('h3')).to_have_text('amity-bedroom settings');expect(d.get_by_role('rowheader').last).to_have_text('4')
            for slot in (0,1,3):expect(input_for(d,slot)).to_be_enabled()
            expect(input_for(d,2)).to_be_disabled()
            expect(d.locator('[data-adapter-id="2"]')).to_contain_text('Not configured')
            expect(d.locator('[data-adapter-id="0"] td').nth(1)).to_have_text('3.0.0.0');expect(d.locator('[data-adapter-id="0"] td').nth(2)).to_have_text('3.0.0.0');expect(input_for(d,0)).to_have_value('2.0.0.0')
            close(d);settings[MEMBER]['adapters'][2]['configured']=False;d=open_settings()
            expect(d.locator('[data-adapter-id="2"]')).to_contain_text('Not configured');expect(input_for(d,2)).to_be_disabled()
            close(d);settings[MEMBER]['adapters'][2]['configured']=True;d=open_settings()
            expect(input_for(d,2)).to_be_enabled();passed('four stable socket numbers; configured wires editable without a sensor; unconfigured defaults read only; observed and saved values separated')
            save=d.get_by_role('button',name='Save',exact=True);start=len(settings_posts())
            for invalid in ('1.0.1.0','F.F.F.F','invalid'):
                input_for(d,3).fill(invalid);save.click();assert len(settings_posts())==start
            passed('invalid topology and reserved unknown address blocked before request')
            input_for(d,3).fill('b.c.0.0');page.wait_for_function('() => !polling');page.evaluate('() => refresh()');expect(input_for(d,3)).to_have_value('b.c.0.0')
            fixture['settings_fail']=True;save.click();expect(d.get_by_role('status')).to_contain_text('Stop activities before saving');expect(input_for(d,3)).to_have_value('b.c.0.0');expect(save).to_be_enabled();assert len(settings_posts())==start+1
            passed('failed quiescence/save preserves edit and controls without automatic retry')
            fixture['settings_fail']=False;captured=deepcopy(settings[MEMBER]);save.click();expect(d.get_by_role('status')).to_have_text('Settings saved.')
            path,body=settings_posts()[-1];assert path==f'/api/devices/{MEMBER}/settings'
            assert body['expected_boot_id']==captured['boot_id'] and body['expected_settings_revision']==captured['settings_revision']
            assert 'confirm_restart' not in body and body['changes']==[dict(tentacle_id=3,default_front_physical_address='B.C.0.0')]
            assert settings[MEMBER]['adapters'][1]['default_front_physical_address']=='4.0.0.0' and settings[MAIN]['settings_revision']=='1'
            expect(page.locator('#admin')).to_be_visible();assert fixture['operations'][next(reversed(fixture['operations']))]['polls']>=2
            assert settings[MEMBER]['boot_id']==captured['boot_id'] and fixture['boot_id']==RENAMED_BOOT
            expect(input_for(d,3)).to_have_value('B.C.0.0');expect(d.locator('[data-adapter-id="3"] td').nth(2)).to_have_text('B.C.0.0');expect(save).to_be_enabled()
            passed('captured decimal CAS and exact slot sent; admission polled to applied; fallback selection updates in the open table without restarting either device')
            # Change again without reopening: the dialog must use the applied revision.
            applied_revision=settings[MEMBER]['settings_revision'];input_for(d,3).fill('A.C.0.0');save.click();expect(d.get_by_role('status')).to_have_text('Settings saved.')
            assert settings_posts()[-1][1]['expected_settings_revision']==applied_revision
            expect(d.locator('[data-adapter-id="3"] td').nth(2)).to_have_text('A.C.0.0')
            # A stored scanned address wins over both equal and different defaults.
            for address in ('3.0.0.0','5.0.0.0'):
                input_for(d,0).fill(address);save.click();expect(d.get_by_role('status')).to_have_text('Settings saved.')
                expect(input_for(d,0)).to_have_value(address)
                expect(d.locator('[data-adapter-id="0"] td').nth(1)).to_have_text('3.0.0.0');expect(d.locator('[data-adapter-id="0"] td').nth(2)).to_have_text('3.0.0.0')
            assert settings[MEMBER]['boot_id']==captured['boot_id'] and settings[MAIN]['settings_revision']=='1'
            passed('success refreshes reviewed revision for another save; detected selection survives both equal and different defaults');close(d)
            d=open_settings();input_for(d,3).fill('');d.get_by_role('button',name='Save',exact=True).click();expect(d.get_by_role('status')).to_have_text('Settings saved.')
            assert settings_posts()[-1][1]['changes'][0]['default_front_physical_address']=='1.0.0.0';passed('blank resets only the selected stable slot to default');close(d)
            # The target may save before its reply disappears. Both an explicit
            # unknown operation and an HTTP failure must require a fresh read,
            # rather than granting a second attempt from the old dialog.
            fixture['settings_outcome']='unknown';d=open_settings();start=len(settings_posts());input_for(d,3).fill('8.0.0.0')
            save=d.get_by_role('button',name='Save',exact=True);save.click()
            expect(d.get_by_role('status')).to_contain_text('save outcome is unknown');expect(save).to_be_disabled()
            input_for(d,3).fill('9.0.0.0');expect(save).to_be_disabled();page.wait_for_timeout(400);assert len(settings_posts())==start+1
            d.get_by_role('button',name='Refresh',exact=True).click();expect(input_for(d,3)).to_have_value('8.0.0.0');expect(save).to_be_enabled()
            assert len(settings_posts())==start+1;close(d);fixture['settings_outcome']='applied'
            fixture['settings_http500']=True;d=open_settings();start=len(settings_posts());input_for(d,3).fill('9.0.0.0')
            save=d.get_by_role('button',name='Save',exact=True);save.click()
            expect(d.get_by_role('status')).to_contain_text('save outcome is unknown');expect(save).to_be_disabled()
            page.wait_for_timeout(400);assert len(settings_posts())==start+1
            fixture['settings_http500']=False;d.get_by_role('button',name='Refresh',exact=True).click();expect(input_for(d,3)).to_have_value('9.0.0.0');expect(save).to_be_enabled()
            assert len(settings_posts())==start+1;passed('unknown or HTTP500 after durable save disables Save until Refresh reviews saved values; neither outcome replays the POST');close(d)
            d=open_settings();captured=deepcopy(settings[MEMBER]);settings[MEMBER]['settings_revision']=str(int(settings[MEMBER]['settings_revision'])+1);input_for(d,3).fill('6.0.0.0');d.get_by_role('button',name='Save',exact=True).click();expect(d.get_by_role('status')).to_contain_text('Settings changed')
            assert settings_posts()[-1][1]['expected_settings_revision']==captured['settings_revision'];expect(input_for(d,3)).to_have_value('6.0.0.0');passed('dialog keeps reviewed settings revision and reports stale-save rejection');close(d)
            d=open_settings();captured=deepcopy(settings[MEMBER]);settings[MEMBER]['boot_id']='d'*32;input_for(d,3).fill('7.0.0.0');d.get_by_role('button',name='Save',exact=True).click();expect(d.get_by_role('status')).to_contain_text('Settings changed')
            assert settings_posts()[-1][1]['expected_boot_id']==captured['boot_id'];passed('member restart rejects the captured boot and never silently retries');close(d)
            fixture['remote_status']='offline';d=open_settings();expect(d.get_by_role('status')).to_contain_text('offline');expect(d.get_by_role('button',name='Save',exact=True)).to_be_disabled()
            for slot in range(4):expect(input_for(d,slot)).to_be_disabled()
            passed('offline cached settings remain visible read only');close(d)
            fixture['remote_status']='ready';fixture['named']=False;d=open_settings();expect(d.locator('h3')).to_have_text('Name unavailable settings');expect(d.get_by_role('button',name='Save',exact=True)).to_be_disabled();passed('unknown hostname cannot masquerade as a named editable device');close(d)
            fixture['named']=True;fixture['remote_status']='unsupported';manage();page.locator('#managedAmityList > article').nth(1).get_by_role('button',name='Settings',exact=True).click();d=page.locator('dialog.adapter-settings:visible');expect(d.get_by_role('status')).to_contain_text('Update this Amity');expect(d.get_by_role('button',name='Save',exact=True)).to_be_disabled();passed('older peer without settings support asks for update without a write');close(d)
            fixture['remote_status']='ready';d=open_settings();assert d.evaluate('(e)=>e.scrollWidth<=e.clientWidth+1')
            for width in (320,430,816):
                page.set_viewport_size(dict(width=width,height=900));assert d.evaluate('(e)=>e.scrollWidth<=e.clientWidth+1')
                assert d.locator('thead th').evaluate_all('''cells=>cells.every(cell=>{
                    let offset=0;return cell.textContent.split(' ').every(word=>{
                        const range=document.createRange();range.setStart(cell.firstChild,offset);range.setEnd(cell.firstChild,offset+word.length);offset+=word.length+1;
                        return range.getClientRects().length===1&&range.getBoundingClientRect().right<=cell.getBoundingClientRect().right;
                    });
                })''')
                page.screenshot(path=str(OUTPUT/f'adapter-settings-{width}.png'),full_page=True)
                expect(d.get_by_role('columnheader')).to_have_text(['Adapter','Default','Detected Address','Selected Address'])
                for slot in range(4):expect(input_for(d,slot)).to_be_enabled()
            page.set_viewport_size(dict(width=430,height=900));page.screenshot(path=str(OUTPUT/'adapter-settings.png'),full_page=True);passed('single accessible address table fits 320px, 430px and desktop viewports');close(d)
            page.locator('[data-tab=advanced]').click();fixture['calls'].clear()
            def cancel(dialog):assert dialog.message=='This will factory reset Amity. You will need to reconfigure it again. Are you sure?';dialog.dismiss()
            page.once('dialog',cancel);page.locator('#factoryReset').click();assert not fixture['calls'];passed('cancelled reset sends no request')
            fixture['fail']=True;page.once('dialog',lambda d:d.accept());page.locator('#factoryReset').click();expect(page.locator('#message')).to_have_text('Save failed');expect(page.locator('#factoryReset')).to_be_enabled();passed('failed reset recovers controls')
            fixture['fail']=False;page.once('dialog',lambda d:d.accept());page.locator('#factoryReset').click();expect(page.locator('#message')).to_contain_text('Factory reset started');expect(page.locator('#admin')).to_be_hidden();assert fixture['calls'][-1]==('/api/factory-reset',{'confirm':True});passed('confirmed reset posts explicit confirmation and ends session')
            assert not errors,errors;browser.close()
    finally:server.shutdown();server.server_close()
    report=dict(passed=checks,checks=len(checks),scope='Shipped browser scripts against local fixtures; no physical HDMI, restart or peer hardware qualification')
    (OUTPUT/'web-device-settings-ui.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Evidence:',OUTPUT,flush=True)
if __name__=='__main__':main()
