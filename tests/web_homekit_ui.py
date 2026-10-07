#!/usr/bin/env python3
"""Exercise the optional HomeKit tab and pairing lifecycle against a local API fixture."""
import json
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Event, Thread
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]

def main():
    state = {'available': False, 'started': False, 'paired': False, 'pairing': False, 'busy': False}
    calls, errors, checks = [], [], []
    gate = Event(); opening = Event()
    qr = ['1' * 33] * 33
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(ROOT / 'components/web_admin/assets'), **kw)
        def log_message(self, *a): pass
        def reply(self, value, code=200):
            data=json.dumps(value).encode(); self.send_response(code)
            self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(data)))
            self.end_headers(); self.wfile.write(data)
        def do_GET(self):
            fixtures={'/api/setup':{'required':False}, '/api/config':{'version':1,'activities':[]},
                '/api/status':{'version':'test','ready':True,'current_activity':-1,'devices':[]},
                '/api/remotes':{'bonded':False},'/api/matter':{'fabrics':[]},'/api/homekit':deepcopy(state)}
            if self.path in fixtures: return self.reply(fixtures[self.path])
            return super().do_GET()
        def do_POST(self):
            body=json.loads(self.rfile.read(int(self.headers.get('Content-Length','0'))))
            calls.append(self.path)
            if self.path=='/api/login': return self.reply({'token':'fixture-session'})
            if self.path=='/api/homekit/open':
                assert body=={}; opening.set(); assert gate.wait(20)
                state.update(started=True,pairing=True,manual_code='234-56-789',qr_modules=qr)
            elif self.path=='/api/homekit/close': state.update(pairing=False)
            elif self.path=='/api/homekit/remove':
                assert body=={'confirm':True}; state.update(paired=False)
            return self.reply({'ok':True})
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    Thread(target=server.serve_forever,daemon=True).start()
    def passed(name): checks.append(name); print('PASS',name,flush=True)
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True)
            try:
                page=browser.new_page(); page.set_default_timeout(10000)
                page.on('pageerror',lambda e:errors.append(str(e)))
                page.goto(f'http://127.0.0.1:{server.server_port}')
                page.locator('#password').fill('fixture-password'); page.locator('#password').press('Enter')
                expect(page.locator('#admin')).to_be_visible()
                page.locator('[data-tab=homekit]').click()
                expect(page.locator('#homekitStatus')).to_have_text('Not paired')
                expect(page.locator('#homekitPairingHint')).to_be_visible()
                expect(page.locator('#homekitPairingHint')).to_have_text('Connect Amity to Wi-Fi through Matter before pairing.')
                expect(page.locator('#toggleHomekit')).to_be_disabled()
                expect(page.locator('#homekitQr')).to_be_hidden(); expect(page.locator('#homekitPairingCode')).to_have_text('')
                passed('separate HomeKit tab requires Wi-Fi provisioning and initially hides setup material')
                state['available']=True; page.evaluate('() => refreshHomekit()')
                expect(page.locator('#homekitStatus')).to_have_text('Not paired')
                expect(page.locator('#homekitPairingHint')).to_be_hidden()
                expect(page.locator('#toggleHomekit')).to_be_enabled(); page.locator('#toggleHomekit').click()
                expect(page.locator('#homekitStatus')).to_have_text('Updating pairing…')
                expect(page.locator('#toggleHomekit')).to_be_disabled()
                assert opening.wait(5); assert calls.count('/api/homekit/open')==1
                passed('pairing preparation has visible progress and rejects duplicate clicks')
                gate.set(); expect(page.locator('#homekitQr svg')).to_be_visible()
                expect(page.locator('#homekitStatus')).to_have_text('Ready to pair')
                expect(page.locator('#homekitPairingHint')).to_be_visible()
                expect(page.locator('#homekitPairingHint')).to_contain_text('In your home app, scan this QR code or enter the pairing code.')
                expect(page.locator('#homekitPairingCode')).to_have_text('Pairing code: 234-56-789')
                expect(page.locator('#toggleHomekit')).to_have_text('Stop pairing')
                passed('starting pairing displays an inline QR and pairing code')
                page.locator('#toggleHomekit').click(); expect(page.locator('#homekitQr')).to_be_hidden()
                expect(page.locator('#homekitStatus')).to_have_text('Not paired')
                expect(page.locator('#homekitPairingHint')).to_be_hidden()
                expect(page.locator('#homekitPairingCode')).to_have_text('')
                passed('closing pairing clears both QR and PIN even if a response contains stale setup material')
                state.update(pairing=True); page.evaluate('() => refreshHomekit()')
                expect(page.locator('#homekitQr')).to_be_visible()
                state.update(pairing=False,paired=True); page.evaluate('() => refreshHomekit()')
                expect(page.locator('#homekitStatus')).to_have_text('Connected')
                expect(page.locator('#homekitQr')).to_be_hidden(); expect(page.locator('#homekitPairingCode')).to_have_text('')
                expect(page.locator('#removeHomekit')).to_be_visible()
                passed('successful pairing hides setup material and offers removal')
                page.on('dialog',lambda dialog:dialog.accept()); page.locator('#removeHomekit').click()
                expect(page.locator('#toggleHomekit')).to_be_enabled()
                expect(page.locator('#homekitStatus')).to_have_text('Not paired')
                expect(page.locator('#homekitPairingHint')).to_be_hidden()
                assert calls.count('/api/homekit/remove')==1
                passed('idle HomeKit shows Not paired before setup and after closing pairing or removing Apple Home')
                state.update(busy=True); page.evaluate('() => refreshHomekit()')
                expect(page.locator('#toggleHomekit')).to_be_disabled()
                expect(page.locator('#homekitStatus')).to_have_text('Updating pairing…')
                state.update(busy=False,error='ESP_FAIL'); page.evaluate('() => refreshHomekit()')
                expect(page.locator('#homekitStatus')).to_contain_text('failed')
                passed('removal, server progress, and operation errors are visible')
                state.pop('error'); state.update(pairing=True); page.evaluate('() => refreshHomekit()')
                page.locator('#logout').click(); expect(page.locator('#login')).to_be_visible()
                expect(page.locator('#homekitPairingCode')).to_have_text('')
                assert page.locator('#homekitQr svg').count()==0
                passed('logging out clears pairing secrets from the page')
                assert not errors,errors; passed('no browser script exceptions')
            finally: browser.close()
    finally: gate.set(); server.shutdown(); server.server_close()
    (ROOT/'test-results/web-homekit-ui.json').write_text(json.dumps({'passed':checks},indent=2)+'\n')

if __name__=='__main__': main()
