#!/usr/bin/env python3
"""Check hostname editing, branding, and factory-reset confirmation against a local API fixture."""
import json
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from playwright.sync_api import sync_playwright, expect
ROOT=Path(__file__).resolve().parents[1]
fixture={'address':'1.0.0.0','hostname':'amity-dut','calls':[],'fail':False}
class Handler(SimpleHTTPRequestHandler):
    def __init__(self,*args,**kwargs):super().__init__(*args,directory=str(ROOT/'components/web_admin/assets'),**kwargs)
    def log_message(self,*args):pass
    def reply(self,data,status=200):
        data=json.dumps(data).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_GET(self):
        values={'/api/setup':{'required':False,'hostname':fixture['hostname']},'/api/config':{'version':1,'activities':[]},
                '/api/status':{'front_physical_address':fixture['address'],'hostname':fixture['hostname'],'version':'test','current_activity':-1,'devices':[]},
                '/api/remotes':{'devices':[]},'/api/matter':{'fabrics':[]}}
        if self.path in values:return self.reply(values[self.path])
        super().do_GET()
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))))
        if self.path=='/api/login':return self.reply({'token':'test'})
        fixture['calls'].append((self.path,data))
        if fixture['fail']:return self.reply({'error':'Save failed'},500)
        if self.path=='/api/hostname':fixture['hostname']=data['hostname'].lower();return self.reply({'hostname':fixture['hostname'],'rebooting':True})
        if self.path=='/api/front-physical-address':fixture['address']=data['address'].upper() or '1.0.0.0';return self.reply({'front_physical_address':fixture['address'],'rebooting':True})
        return self.reply({'ok':True})
def main():
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler);Thread(target=server.serve_forever,daemon=True).start()
    checks=[]
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True);page=browser.new_page();errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
            def enter():
                page.goto(f'http://127.0.0.1:{server.server_port}/');page.locator('#password').fill('test');page.locator('#password').press('Enter');expect(page.locator('#admin')).to_be_visible();page.locator('[data-tab=advanced]').click()
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            expect(page.locator('h1')).to_have_text('amity-dut');expect(page).to_have_title('amity-dut');expect(page.locator('header .byline')).to_have_count(0);expect(page.locator('footer')).to_have_text('Amity - HDMI-CEC that works!');checks.append('hostname header before login; footer replaces byline')
            enter();expect(page.locator('#hostname')).to_have_value('amity-dut')
            page.locator('#hostname').fill('Living-Room');page.wait_for_function('() => !polling');page.evaluate('() => refresh()');expect(page.locator('#hostname')).to_have_value('Living-Room');checks.append('polling preserves edited hostname')
            fixture['fail']=True;page.locator('#changeHostname button').click();expect(page.locator('#message')).to_have_text('Save failed');expect(page.locator('#admin')).to_be_visible();checks.append('failed hostname save stays editable')
            fixture['fail']=False;page.locator('#changeHostname button').click();expect(page.locator('#hostnameLink')).to_be_visible();expect(page.locator('#hostnameLink')).to_have_attribute('href','https://living-room.local');expect(page.locator('h1')).to_have_text('living-room');expect(page.locator('#admin')).to_be_hidden();checks.append('successful hostname change gives new URL and ends old session')
            enter();expect(page.locator('#frontPhysicalAddress')).to_have_value('1.0.0.0')
            page.locator('#frontPhysicalAddress').fill('invalid');page.locator('#changeFrontPhysicalAddress button').click();assert not any(path=='/api/front-physical-address' for path,data in fixture['calls']);checks.append('malformed physical address blocked before request')
            page.locator('#frontPhysicalAddress').fill('2.0.0.0');fixture['fail']=True;page.locator('#changeFrontPhysicalAddress button').click();expect(page.locator('#message')).to_have_text('Save failed');expect(page.locator('#frontPhysicalAddress')).to_have_value('2.0.0.0');expect(page.locator('#changeFrontPhysicalAddress button')).to_be_enabled();checks.append('failed physical address save preserves edit and controls')
            fixture['fail']=False;page.locator('#changeFrontPhysicalAddress button').click();expect(page.locator('#admin')).to_be_hidden();assert fixture['calls'][-1]==('/api/front-physical-address',{'address':'2.0.0.0'});checks.append('physical address saved and session ends for reboot')
            enter();expect(page.locator('#frontPhysicalAddress')).to_have_value('2.0.0.0');page.locator('#frontPhysicalAddress').fill('');page.locator('#frontPhysicalAddress').blur();page.wait_for_function('() => !polling');page.evaluate('() => refresh()');expect(page.locator('#frontPhysicalAddress')).to_have_value('');checks.append('polling preserves deliberately empty physical address')
            page.locator('#changeFrontPhysicalAddress button').click();expect(page.locator('#admin')).to_be_hidden();assert fixture['calls'][-1]==('/api/front-physical-address',{'address':''});enter();expect(page.locator('#frontPhysicalAddress')).to_have_value('1.0.0.0');checks.append('empty physical address resets to default')
            fixture['calls'].clear()
            def cancel(dialog):assert dialog.message=='This will factory reset Amity. You will need to reconfigure it again. Are you sure?';dialog.dismiss()
            page.once('dialog',cancel);page.locator('#factoryReset').click();assert not fixture['calls'];checks.append('cancelled reset sends no request')
            fixture['fail']=True;page.once('dialog',lambda d:d.accept());page.locator('#factoryReset').click();expect(page.locator('#message')).to_have_text('Save failed');expect(page.locator('#factoryReset')).to_be_enabled();checks.append('failed reset recovers controls')
            fixture['fail']=False;page.once('dialog',lambda d:d.accept());page.locator('#factoryReset').click();expect(page.locator('#message')).to_contain_text('Factory reset started');expect(page.locator('#admin')).to_be_hidden();assert fixture['calls'][-1]==('/api/factory-reset',{'confirm':True});checks.append('confirmed reset posts explicit confirmation and ends session')
            assert not errors,errors;browser.close()
    finally:server.shutdown();server.server_close()
    (ROOT/'test-results/web-device-settings-ui.json').write_text(json.dumps({'passed':checks},indent=2))
    for check in checks:print('PASS',check)
if __name__=='__main__':main()
