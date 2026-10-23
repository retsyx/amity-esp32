#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Verify optional HomeKit over encrypted IP with aiohomekit (isolated test dependency).

Use only on an authorized test DUT with Matter already commissioned. The test
uses a temporary HomeKit controller, restores names/visibility and removes its
pairings afterward. Credentials and controller keys are written privately.
"""
import argparse
import asyncio
import json
import os
import time
import urllib.error
from pathlib import Path
from zeroconf.asyncio import AsyncZeroconf, AsyncServiceInfo
from aiohomekit.characteristic_cache import CharacteristicCacheMemory
from aiohomekit.controller.ip.controller import IpController
from aiohomekit.controller.ip.discovery import IpDiscovery
from aiohomekit.protocol.tlv import TLV
from aiohomekit.zeroconf import HomeKitService
from aiohomekit.exceptions import UnavailableError, AuthenticationError, HttpErrorResponse
from cec_web_activity import Admin, write_private

async def security_regressions(pairing,admin,readable_iid):
    boot_id=admin.request('/api/setup')['boot_id']
    async def healthy(protocol):
        result=await pairing.get_characteristics([(1,readable_iid)])
        assert 'value' in result[(1,readable_iid)],result
        assert pairing.connection.protocol is protocol,'Malformed request interrupted the encrypted connection'
        assert admin.request('/api/setup')['boot_id']==boot_id,'Malformed request restarted the DUT'
    for invalid_id in ['', '1', '1.', '.1', 'nonnumeric', 'one.two', '1.two', '0.1', '1.0',
                       '2147483648.1', '1.2147483648', '1.2,', '1.2,,3.4', '1.2,3', '1.2.3']:
        await pairing._ensure_connected()
        protocol=pairing.connection.protocol
        assert pairing.connection.is_secure,'Regression request must use an encrypted session'
        try:
            response=await asyncio.wait_for(pairing.connection.get('/characteristics?id='+invalid_id),10)
        except HttpErrorResponse as error:
            response=error.response
        assert response.code==400,('Malformed characteristic ID accepted',invalid_id,response.code)
        assert json.loads(response.body)=={'status':-70409},(invalid_id,response.body)
        await healthy(protocol)

    peers=await pairing.list_pairings()
    assert len(peers)==1 and peers[0]['pairingId']==pairing._pairing_data['iOSPairingId'],peers
    key=b'\x42'*32
    # Encode directly: aiohomekit's TLV encoder omits empty fields, which would
    # skip the zero-length-field cases that previously used uninitialized data.
    def encode(fields):
        return b''.join(bytes((kind,len(value)))+value for kind,value in fields)
    cases=[('64-byte identifier',b'a'*64,key,b'\x00',TLV.AddPairing),
           ('empty identifier',b'',key,b'\x00',TLV.AddPairing),
           ('embedded NUL identifier',b'regression\0peer',key,b'\x00',TLV.AddPairing),
           ('31-byte public key',b'regression-peer',key[:-1],b'\x00',TLV.AddPairing),
           ('empty public key',b'regression-peer',b'',b'\x00',TLV.AddPairing),
           ('empty permissions',b'regression-peer',key,b'',TLV.AddPairing),
           ('oversized permissions',b'regression-peer',key,b'\x00\x01',TLV.AddPairing),
           ('invalid permissions',b'regression-peer',key,b'\x02',TLV.AddPairing),
           ('64-byte removal identifier',b'a'*64,key,b'\x00',TLV.RemovePairing),
           ('empty removal identifier',b'',key,b'\x00',TLV.RemovePairing)]
    for name,identifier,public_key,permissions,method in cases:
        fields=[(TLV.kTLVType_State,TLV.M1),(TLV.kTLVType_Method,method),
                (TLV.kTLVType_Identifier,identifier)]
        if method==TLV.AddPairing:
            fields.extend([(TLV.kTLVType_PublicKey,public_key),(TLV.kTLVType_Permissions,permissions)])
        protocol=pairing.connection.protocol
        response=await asyncio.wait_for(pairing.connection.post('/pairings',encode(fields)),10)
        assert response.code==200,(name,response.code)
        result=dict(TLV.decode_bytes(response.body))
        assert result.get(TLV.kTLVType_State)==TLV.M2,(name,result)
        assert result.get(TLV.kTLVType_Error)==TLV.kTLVError_Unknown,(name,result)
        assert await pairing.list_pairings()==peers,('Malformed pairing request changed controller list',name)
        await healthy(protocol)

async def run(args):
    admin=Admin(args.url); password=json.loads(args.credentials.read_text())['admin_password']
    admin.login(password)
    original={k:admin.request('/api/'+k) for k in ['config','matter','remotes','status','homekit']}
    assert original['homekit']['available'], 'Provision Wi-Fi through Matter first'
    assert not original['homekit']['paired'], 'Refusing to replace an existing HomeKit association'
    checks=[]; pairing=None; renamed=[]; zc=AsyncZeroconf(); controller=IpController(CharacteristicCacheMemory(),zc)
    def passed(name): checks.append(name); print('PASS',name,flush=True)
    async def wait_homekit(predicate,timeout=45):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            s=admin.request('/api/homekit')
            if predicate(s): return s
            await asyncio.sleep(.3)
        raise AssertionError('Timed out waiting for HomeKit state')
    async def operation(name):
        admin.request('/api/homekit/'+name,{'confirm':True} if name=='remove' else {})
        return await wait_homekit(lambda s:not s['busy'])
    async def put(iid,value):
        result=await pairing.put_characteristics([(1,iid,value)])
        assert not result,result
    async def get(iid):
        return (await pairing.get_characteristics([(1,iid)]))[(1,iid)]
    async def wait_activity(index):
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if admin.request('/api/status')['current_activity']==index:return
            await asyncio.sleep(.25)
        raise AssertionError('HomeKit activity did not reach observed state')
    short=lambda uuid:format(int(uuid.split('-')[0],16),'X')
    try:
        try: Admin(args.url).request('/api/homekit'); raise AssertionError('Unauthenticated HomeKit API allowed')
        except urllib.error.HTTPError as e: assert e.code==401
        passed('HomeKit API requires authentication')
        await operation('close'); first=await operation('open')
        assert first['pairing'] and first['manual_code'] and first['qr_payload'].startswith('X-HM://')
        write_private(args.output.with_suffix('.setup.json'), first)
        payload=first['qr_payload'];assert len(payload)==20
        bits=int(payload[7:16],36)
        assert (bits&0x7ffffff)==int(first['manual_code'].replace('-',''))
        assert (bits>>31)&255==31 and (bits>>27)&15==2 and bits>>39==0
        info=AsyncServiceInfo('_hap._tcp.local.','Amity._hap._tcp.local.')
        assert await info.async_request(zc.zeroconf,10000),'HomeKit mDNS service missing'
        description=HomeKitService.from_service_info(info)
        assert int(description.category)==31 and description.port==51826
        assert info.server.lower()==original['status']['hostname'].lower()+'.local.'
        passed('HomeKit advertises television category using the shared Amity hostname')
        closed=await operation('close'); assert not closed['pairing'] and 'manual_code' not in closed and 'qr_payload' not in closed
        rejected=IpDiscovery(controller,description)
        try:
            try: await rejected.async_start_pairing('forbidden'); raise AssertionError('Closed pairing accepted')
            except AssertionError: raise
            except UnavailableError: pass
        finally: await rejected.close()
        second=await operation('open'); assert second['manual_code']!=first['manual_code']
        passed('closing gates protocol pairing; reopening rotates PIN and hides closed setup material')
        unfinished=IpDiscovery(controller,description)
        try:
            old_finish=await unfinished.async_start_pairing('cancelled-controller')
            await operation('close'); replacement=await operation('open')
            try:
                await old_finish(second['manual_code']); raise AssertionError('Cancelled pairing was accepted')
            except AssertionError: raise
            except UnavailableError: pass
            second=replacement
        finally: await unfinished.close()
        passed('closing and reopening rejects an incomplete pairing from the old window')
        wrong_pin=IpDiscovery(controller,description)
        try:
            wrong_finish=await wrong_pin.async_start_pairing('wrong-pin-controller')
            invalid='234-56-789' if second['manual_code']!='234-56-789' else '345-67-890'
            try:
                await wrong_finish(invalid);raise AssertionError('Wrong PIN was accepted')
            except AuthenticationError: pass
        finally:await wrong_pin.close()
        passed('incorrect HomeKit PIN is rejected without creating a controller')
        discovery=IpDiscovery(controller,description)
        try:
            finish=await discovery.async_start_pairing('amity-test-controller')
            pairing=await asyncio.wait_for(finish(second['manual_code']),50)
        finally: await discovery.close()
        write_private(args.output.with_suffix('.controller.json'),pairing._pairing_data)
        await wait_homekit(lambda s:s['paired'] and not s['pairing'] and 'manual_code' not in s)
        passed('SRP pairing succeeds and clears QR/PIN after completion')
        data=await pairing.list_accessories_and_characteristics()
        write_private(args.output.with_suffix('.accessories.json'),data)
        assert len(data)==1 and data[0]['aid']==1
        services={}
        for s in data[0]['services']: services.setdefault(short(s['type']),[]).append(s)
        assert len(services['D8'])==len(services['113'])==len(services['96'])==1
        assert len(services['D9'])==16
        tv=services['D8'][0]; speaker=services['113'][0]; battery=services['96'][0]
        chars=lambda s:{short(c['type']):c for c in s['characteristics']}
        tc,sc,bc=chars(tv),chars(speaker),chars(battery)
        assert tv.get('primary') and {'B0','E7','E3','E1','E8'}<=tc.keys()
        assert {'11A','B0','E9','EA'}<=sc.keys() and {'68','79','8F'}<=bc.keys()
        assert sc['E9']['value']==1
        assert speaker['iid'] in tv['linked']
        for index,s in enumerate(services['D9']):
            ch=chars(s); assert {'E6','E3','DB','D6','23','135','134'}<=ch.keys()
            assert s['iid'] in tv['linked'] and ch['E6']['value']==index+1
            exists=index<len(original['config']['activities'])
            assert ch['D6']['value']==int(exists)
            if exists: assert ch['E3']['value']==original['config']['activities'][index]['name']
            else: assert ch['135']['value']==1
        passed('Television, linked speaker, saved activity inputs, hidden spare slots and battery services are complete')
        await security_regressions(pairing,admin,tc['E3']['iid'])
        passed('malformed encrypted characteristic IDs return HTTP 400 while valid reads and boot remain stable')
        passed('malformed authenticated pairing TLVs are rejected without changing controllers or interrupting HomeKit')
        for uuid in ['68','79','8F']:
            result=await get(bc[uuid]['iid'])
            remote=admin.request('/api/remotes')
            if remote.get('bonded') and not remote.get('connected'): assert result.get('status')==-70409,result
            elif remote.get('connected') and uuid=='68': assert result['value']==remote['battery'],result
        passed('battery readings match the shared BLE state, with unavailable readings when disconnected')
        # Real writes, reads and event notifications, followed by a boot using saved controller keys.
        events=[]; pairing.dispatcher_connect(lambda values:events.append(values))
        await pairing.subscribe([(1,tc['B0']['iid']),(1,tc['E3']['iid'])])
        name=tc['E3']['value']; renamed.append((tc['E3']['iid'],name)); await put(tc['E3']['iid'],'Amity HAP Fixture')
        assert (await get(tc['E3']['iid']))['value']=='Amity HAP Fixture'
        first_input=chars(services['D9'][0]); renamed.append((first_input['E3']['iid'],first_input['E3']['value']))
        await put(first_input['E3']['iid'],'Fixture Input'); await put(first_input['134']['iid'],1)
        assert (await get(first_input['135']['iid']))['value']==1
        if args.exercise_controls:
            assert original['config']['activities'], 'An existing fixture activity is required'
            await put(tc['B0']['iid'],1); await wait_activity(0); await asyncio.sleep(8)
            assert (await get(tc['B0']['iid']))['value']==1
            await put(tc['E1']['iid'],4); await put(tc['E1']['iid'],11); await put(tc['E1']['iid'],11); await put(sc['EA']['iid'],0)
            await asyncio.sleep(2)
            await put(tc['B0']['iid'],0); await wait_activity(-1); await asyncio.sleep(8)
            assert (await get(tc['B0']['iid']))['value']==0
            passed('HomeKit power, navigation and relative speaker controls reach the shared hub')
        for _ in range(20):
            if events:break
            await asyncio.sleep(.1)
        assert events,'No encrypted event notifications received'
        passed('renames, input visibility and encrypted notifications work')
        if args.reboot:
            await pairing.close(); old_boot=admin.request('/api/setup')['boot_id']; admin.request('/api/reboot',{})
            await asyncio.sleep(4); deadline=time.monotonic()+90
            while time.monotonic()<deadline:
                try:
                    boot=admin.request('/api/setup')
                    if boot['boot_id']!=old_boot and boot['ready']:
                        admin.login(password);break
                except OSError: pass
                await asyncio.sleep(1)
            else: raise AssertionError('DUT did not restart')
            assert admin.request('/api/homekit')['paired']
            pairing=controller.load_pairing('resumed',json.loads(args.output.with_suffix('.controller.json').read_text()))
            assert (await get(tc['E3']['iid']))['value']=='Amity HAP Fixture'
            assert (await get(first_input['E3']['iid']))['value']=='Fixture Input'
            assert (await get(first_input['135']['iid']))['value']==1
            passed('HomeKit controller identity, renamed inputs and visibility survive reboot')
        for iid,value in renamed:await put(iid,value)
        renamed.clear(); await put(first_input['134']['iid'],first_input['134']['value'])
        assert await pairing.remove_pairing(pairing._pairing_data['iOSPairingId'])
        await wait_homekit(lambda s:not s['paired'] and not s['pairing'])
        passed('controller removal closes HomeKit without reopening pairing')
        await pairing.close();pairing=None
        after={k:admin.request('/api/'+k) for k in ['config','matter','remotes','status']}
        assert after['config']==original['config'] and after['matter']['fabrics']==original['matter']['fabrics']
        assert after['remotes']['bonded']==original['remotes']['bonded'] and after['status']['hostname']==original['status']['hostname']
        passed('HomeKit lifecycle preserves Matter, Wi-Fi, saved activities and BLE bond')
        write_private(args.output,{'passed':checks,'status':after['status']})
    finally:
        if pairing:
            for iid,value in renamed:
                try: await put(iid,value)
                except Exception: pass
            await pairing.close()
        try:
            if admin.request('/api/homekit')['paired']: await operation('remove')
            await operation('close')
            if admin.request('/api/status')['current_activity']!=original['status']['current_activity']:
                admin.request('/api/activity',{'index':original['status']['current_activity']})
        finally: await zc.async_close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--url',required=True)
    p.add_argument('--credentials',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reboot',action='store_true');p.add_argument('--exercise-controls',action='store_true')
    args=p.parse_args();os.umask(0o077);args.output.parent.mkdir(parents=True,exist_ok=True)
    asyncio.run(run(args))

if __name__=='__main__':main()
