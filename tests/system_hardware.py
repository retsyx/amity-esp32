#!/usr/bin/env python3
"""Validate schema-2 migration and coordinator-to-wire execution on the isolated fixture.

Uses existing stable activities, restores the portable configuration, and never
resets the DUT, changes pairings or reuses an external selector for another ID.
"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import shlex
import subprocess
import time
import uuid
import urllib.error
from cec_web_activity import Admin,read_password,write_private
from cec_hardware import Harness,raw_frames


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True);parser.add_argument('--credentials',type=Path,required=True)
    parser.add_argument('--host',default='pi@10.0.1.119');parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();admin=Admin(args.url);harness=Harness(args.host);report=dict(passed=[],api=admin.records,harness=harness.records)
    original=None;monitors=[]
    def check(name,condition):
        if not condition:raise AssertionError(name)
        report['passed'].append(name);print('PASS',name,flush=True)
    def mutation(path,**body):return admin.request('/api/v2/'+path,dict(request_id=uuid.uuid4().hex,**body))
    def wait_operation(operation,timeout=60):
        deadline=time.monotonic()+timeout
        while operation['stage'] in ('admitted','configuring','activating','executing','preparing','stopping') and time.monotonic()<deadline:
            time.sleep(.3);operation=admin.request('/api/v2/operations/'+operation['operation_id'])
        report.setdefault('operations',[]).append(operation)
        return operation
    def wait_system(predicate,timeout=45):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=admin.request('/api/v2/system');report['last_state']=state
            if predicate(state):return state
            time.sleep(.5)
        raise AssertionError('Timed out waiting for schema-2 runtime')
    def save(candidate):
        current=admin.request('/api/v2/config')
        op=mutation('config',configuration=candidate,expected_config_revision=current['config_revision'],expected_presentation_revision=current['presentation_revision'])
        result=wait_operation(op);check('configuration is applied by owner',result['stage']=='applied')
        return admin.request('/api/v2/config')
    def monitor(seconds):
        result=[]
        for adapter in ('/dev/cec0','/dev/cec1'):
            command=shlex.join(['sudo','-n','cec-ctl','-d',adapter,'-s','-r','-v','-M','--monitor-time',str(seconds)])
            process=subprocess.Popen(['ssh','-o','BatchMode=yes','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null',args.host,command],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            monitors.append(process);result.append((adapter,process))
        time.sleep(.4);return result
    def capture(monitored):
        result={}
        for adapter,process in monitored:
            out,err=process.communicate(timeout=70);result[adapter]=out
            if process.returncode or 'Selecting monitor mode failed' in err:raise AssertionError('Harness monitor failed: '+err)
        report.setdefault('captures',[]).append(result);return {adapter:raw_frames(text) for adapter,text in result.items()}
    try:
        admin.login(read_password(args.credentials));original=admin.request('/api/v2/config')
        write_private(args.output.with_name(args.output.stem+'-original.json'),original)
        state=wait_system(lambda s:not s['discovering'] and s['ready'],90)
        check('schema2 durable strong identities',original['version']==2 and len(original['consortium_id'])==32 and len({a['activity_id'] for a in original['activities']})==len(original['activities']))
        check('one shared timer for two independent CEC ports',state['cec_runtime']['active_ports']==2 and state['cec_runtime']['timer_instances']==1)
        check('restart does not resume saved activities',not state['sessions'])
        check('runtime LA is absent from portable configuration','logical_address' not in json.dumps(original))
        check('fixture peers remain named native observations',{'HarnessTV','HarnessSource'} <= {o['title'] for o in state['observations'] if o['resolved']})
        activity=next(a for a in original['activities'] if a['label']=='Watch HarnessSource')
        if not activity['enabled']:
            editor=admin.request('/api/v2/activity-editor')
            before_ids={a['activity_id'] for a in original['activities']};before_slots=deepcopy(original['external_slots'])
            op=mutation('activity-editor',version=1,expected_config_revision=editor['expected_config_revision'],expected_presentation_revision=editor['expected_presentation_revision'],activities=editor['activities'],quirks=editor['quirks'])
            result=wait_operation(op);check('original activity editor repairs the observed fixture route',result['stage']=='applied')
            original=admin.request('/api/v2/config');activity=next(a for a in original['activities'] if a['activity_id']==activity['activity_id'])
            check('repair preserves logical activity identities and selectors',activity['enabled'] and {a['activity_id'] for a in original['activities']}==before_ids and original['external_slots']==before_slots)
            write_private(args.output.with_name(args.output.stem+'-original.json'),original)
        before_slots=deepcopy(original['external_slots']);before_selectors=[c['qualified_names'] for c in original['components']]
        before_bond=admin.request('/api/remotes')
        after_bond=admin.request('/api/remotes')
        report['remote_bond_presence']={'before':before_bond.get('bonded'),'after':after_bond.get('bonded'),'identity_exposed':False}
        check('local remote status preserves bond presence without opening pairing',isinstance(before_bond.get('bonded'),bool) and before_bond['bonded']==after_bond.get('bonded'))
        report['physical_remote_input']='Not exercised by this activity/CEC fixture; use a paired remote for separate physical qualification.'
        protected=monitor(40)
        renamed=deepcopy(original);renamed['octopuses'][0]['label']='Hardware validation';renamed['octopuses'][0]['label_override']=True
        renamed=save(renamed)
        check('cosmetic edit advances presentation only',renamed['config_revision']==original['config_revision'] and int(renamed['presentation_revision'])>int(original['presentation_revision']))
        check('cosmetic edit keeps slots and exact selectors',renamed['external_slots']==before_slots and [c['qualified_names'] for c in renamed['components']]==before_selectors)
        prior=admin.request('/api/v2/system')['discovery_revision'];scan=wait_operation(mutation('activity-editor/discover'))
        check('explicit equipment scan completes',scan['stage']=='complete')
        wait_system(lambda s:not s['discovering'] and s['discovery_revision']!=prior)
        harness.cec('/dev/cec1','--active-source','phys-addr=1.0.0.0')
        frames=capture(protected);forbidden={0x04,0x0d,0x36,0x44,0x45,0x80,0x81,0x82,0x86}
        check('source capture contains the injected active-source stimulus',any(len(f)>1 and f[0]==0x4f and f[1]==0x82 for f in frames['/dev/cec1']))
        check('save, scan and unsolicited source activity have zero protected egress',not any(len(f)>1 and f[0]>>4 in ({4,8,11} if adapter=='/dev/cec0' else {0}) and f[1] in forbidden for adapter,fs in frames.items() for f in fs))
        executing=monitor(18);state=admin.request('/api/v2/system')
        body=dict(request_id=uuid.uuid4().hex,activity_id=activity['activity_id'],expected_config_revision=renamed['config_revision'],expected_state_revision=state['state_revision'],expected_boot_id=state['boot_id'])
        op=admin.request('/api/v2/activity-editor/activity',body);op=wait_operation(op)
        check('explicit stable activity is executed by planner',op['stage']=='complete')
        duplicate=admin.request('/api/v2/activity-editor/activity',body)
        check('duplicate request returns retained operation',duplicate['operation_id']==op['operation_id'])
        for legacy in ('/api/key','/api/activity'):
            try:admin.request(legacy,dict(key=0,index=0));raise AssertionError('Unscoped legacy control was accepted')
            except urllib.error.HTTPError as error:check('unscoped legacy control rejects '+legacy,error.code==409)
        time.sleep(1)
        frames=capture(executing);front=frames['/dev/cec0'];back=frames['/dev/cec1']
        check('guarded route announcement reaches screen domain',any(len(f)>1 and f[0]>>4 in {4,8,11} and f[1]==0x82 for f in front))
        old_pair=(original['config_revision'],original['presentation_revision'])
        try:mutation('config',configuration=original,expected_config_revision=old_pair[0],expected_presentation_revision=old_pair[1]);raise AssertionError('Stale CAS was accepted')
        except urllib.error.HTTPError as error:check('stale edits reject without overwriting config',error.code==409)
        state=admin.request('/api/v2/system');wait_operation(mutation('activity-editor/activity',activity_id=None,expected_config_revision=renamed['config_revision'],expected_state_revision=state['state_revision'],expected_boot_id=state['boot_id']))
    except Exception as error:
        report['error']=str(error);raise
    finally:
        for process in monitors:
            if process.poll() is None:process.terminate()
        if original is not None:
            try:
                current=admin.request('/api/v2/config');state=admin.request('/api/v2/system')
                if state['sessions']:
                    wait_operation(mutation('activity-editor/activity',activity_id=None,expected_config_revision=current['config_revision'],expected_state_revision=state['state_revision'],expected_boot_id=state['boot_id']))
                save(original);report['configuration_restored']=True
            except Exception as error:report['restore_error']=str(error)
        try:admin.request('/api/logout',{})
        except Exception:pass
        write_private(args.output,report)


if __name__=='__main__':main()
