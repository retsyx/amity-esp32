#!/usr/bin/env python3
"""Validate current qualified-lookup schemas, examples, links and package hashes.

Full checks use jsonschema/referencing; hardware-capacity-only uses stdlib.
No network or firmware/device operation occurs.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent

def require(condition, message):
    if not condition:
        raise AssertionError(message)

def read(relative):
    return json.loads((ROOT / relative).read_text())

def check_hardware_capacity(schemas):
    """Static schemas and Kconfig must mirror the shared firmware capacity."""
    repo = ROOT.parent
    header = repo / 'components/amity_system/include/amity/hardware_limits.hpp'
    limits = re.findall(r'^#define\s+AMITY_MAX_TENTACLES\s+([1-9][0-9]*)\s*$',
                        header.read_text(), re.M)
    require(len(limits) == 1, 'Missing or ambiguous canonical hardware capacity')
    maximum = int(limits[0])
    last_slot = maximum - 1
    by_name = {schema['$id'].rsplit('/', 1)[-1]: schema for schema in schemas}
    core = by_name['core-types.schema.json']['$defs']
    settings = by_name['adapter-settings.schema.json']['$defs']
    for name in ('CecPortRef', 'FullyQualifiedCecAddress', 'QualifiedDeviceName'):
        require(core[name]['properties']['tentacle_id']['maximum'] == last_slot,
                f'{name} connector bound differs from canonical hardware capacity')
    for name in ('Change', 'Adapter'):
        require(settings[name]['properties']['tentacle_id']['maximum'] == last_slot,
                f'Settings {name} connector bound differs from canonical hardware capacity')
    for name, field in (('Snapshot', 'adapters'), ('WriteCore', 'changes'), ('WriteRequest', 'changes')):
        require(settings[name]['properties'][field]['maxItems'] == maximum,
                f'Settings {name} array bound differs from canonical hardware capacity')
    require(settings['Adapter']['properties']['name']['enum'] ==
            [f'HDMI adapter {slot + 1}' for slot in range(maximum)],
            'Settings labels differ from canonical hardware capacity')
    labels = {rule['if']['properties']['tentacle_id']['const']:
              rule['then']['properties']['name']['const']
              for rule in settings['Adapter']['allOf']
              if 'tentacle_id' in rule['if']['properties']}
    require(labels == {slot: f'HDMI adapter {slot + 1}' for slot in range(maximum)},
            'Settings label rules differ from canonical hardware capacity')

    production = json.loads((repo / 'components/consortium/protocol-v1.schema.json').read_text())['$defs']
    hello = production['Hello']
    require(hello['properties']['supported_tentacles']['maximum'] == maximum,
            'Peer capacity bound differs from canonical hardware capacity')
    require(hello['properties']['configured_tentacles_mask']['maximum'] == (1 << maximum) - 1,
            'Peer connector mask differs from canonical hardware capacity')
    for rule in hello['allOf']:
        capacity = rule['if']['properties']['supported_tentacles']['const']
        require(1 <= capacity <= maximum and
                rule['then']['properties']['configured_tentacles_mask']['maximum'] == (1 << capacity) - 1,
                'Peer conditional connector mask differs from its advertised capacity')
    for name, field in (('DeviceSettingsSnapshot', 'adapters'), ('DeviceSettingsWrite', 'changes')):
        array = production[name]['properties'][field]
        require(array['maxItems'] == maximum and
                array['items']['properties']['tentacle_id']['maximum'] == last_slot,
                f'Peer {name} bounds differ from canonical hardware capacity')

    kconfig = (repo / 'main/Kconfig.projbuild').read_text()
    profiles = re.findall(r'AMITY_BOARD_DEVELOPMENT_([0-9]+)T\b', kconfig)
    require(profiles and all(int(count) == maximum for count in profiles),
            'Kconfig development profile differs from canonical hardware capacity')
    menus = [(int(label), int(slot)) for label, slot in
             re.findall(r'^menu "HDMI adapter ([0-9]+) \(stable connector ([0-9]+)\)"$', kconfig, re.M)]
    require(menus == [(slot + 1, slot) for slot in range(maximum)],
            'Kconfig connector menus differ from canonical hardware capacity')
    fields = ('FRONT_GPIO', 'BACK_GPIO')
    expected = {f'AMITY_TENTACLE{slot}_{field}' for slot in range(maximum) for field in fields}
    configured = re.findall(r'^config (AMITY_TENTACLE[0-9]+_[A-Z_]+)$', kconfig, re.M)
    require(len(configured) == len(expected) and set(configured) == expected,
            'Kconfig connector fields differ from canonical hardware capacity')
    require(all(int(slot) < maximum for slot in re.findall(r'AMITY_TENTACLE([0-9]+)_', kconfig)),
            'Kconfig references a connector beyond canonical hardware capacity')
    require(re.search(r'^\s*default AMITY_BOARD_LEGACY_1T\s*$', kconfig, re.M),
            'The default Kconfig board profile must retain one connector')

    spec = importlib.util.spec_from_file_location('amity_contract_hardware_profile',
                                                  repo / 'tools/hardware_profile.py')
    hardware = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hardware)
    require(hardware.MAX_TENTACLES == maximum and hardware.MAX_CEC_PORTS == 2 * maximum,
            'Release tooling limits differ from canonical hardware capacity')
    require(hardware.DEVELOPMENT == f'esp32s3-development-{maximum}t-v1' and
            hardware.BOARD_FLAGS[1] == f'AMITY_BOARD_DEVELOPMENT_{maximum}T',
            'Release tooling profile differs from canonical hardware capacity')
    return maximum


def validate_configuration(value):
    owners = {item['octopus_id']: item for item in value['octopuses']}
    components = {item['component_id']: item for item in value['components']}
    activities = {item['activity_id']: item for item in value['activities']}
    zones = {item['zone_id']: item for item in value['zones']}
    for key, items in [('octopus_id', value['octopuses']), ('component_id', value['components']), ('activity_id', value['activities']), ('zone_id', value['zones'])]:
        require(len({item[key] for item in items}) == len(items), 'Duplicate ' + key)
    require(value['primary_zone_id'] in zones, 'Missing primary screen group')
    require(value.get('controller_octopus_id') in owners, 'Missing original controller')
    selectors = set()
    for item in components.values():
        if not item['qualified_names']:
            original=item['generated_label'] or item['label']
            legal=bool(original) and len(original.encode())<=14 and not any(ord(c)<32 or ord(c)==127 for c in original)
            require(item['kind']=='placeholder' and bool(original) and not legal,'Representable equipment requires one scoped OSD selector')
        for name in item['qualified_names']:
            require(name['consortium_id'] == value['consortium_id'] and name['octopus_id'] in owners, 'Foreign selector scope')
            require(name['tentacle_id'] < owners[name['octopus_id']]['supported_tentacles'], 'Selector exceeds owner capacity')
            require(0 < len(name['osd_name'].encode()) <= 14 and not any(ord(c)<32 or ord(c)==127 for c in name['osd_name']), 'Invalid OSD bytes')
            key = tuple(name[k] for k in ['consortium_id','octopus_id','tentacle_id','side','osd_name'])
            require(key not in selectors, 'Duplicate equipment selector')
            selectors.add(key)
    for item in activities.values():
        require(item['zone_id'] in zones, 'Missing activity screen group')
        require(all(item[k] is None or item[k] in components for k in ['source','display','audio','switch_device']), 'Missing role component')
    for zone in zones.values():
        require(set(zone['activity_order']) == {a['activity_id'] for a in activities.values() if a['zone_id']==zone['zone_id']}, 'Screen order loses/duplicates activity')
    require(len({s['slot'] for s in value['external_slots']}) == len(value['external_slots']), 'Duplicate external slot')
    require(all(s['tombstone'] or s['activity_id'] in activities for s in value['external_slots']), 'Missing assigned activity')
    return len(selectors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hardware-capacity-only', action='store_true')
    args = parser.parse_args()
    schemas = [json.loads(p.read_text()) for p in sorted((ROOT/'schemas').glob('*.schema.json'))]
    maximum = check_hardware_capacity(schemas)
    if args.hardware_capacity_only:
        print(json.dumps({'schemas_checked':len(schemas),'hardware_capacity_consistency_checked':True,'max_tentacles':maximum,'max_cec_ports':2*maximum},indent=2))
        return
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
    production = json.loads((ROOT.parent/'components/consortium/protocol-v1.schema.json').read_text())
    for value in schemas+[production]: Draft202012Validator.check_schema(value)
    registry = Registry().with_resources((value['$id'],Resource.from_contents(value)) for value in schemas+[production])
    validators = {p['$id'].rsplit('/',1)[-1]:Draft202012Validator(p,registry=registry) for p in schemas}
    def valid(name, value): return validators[name].is_valid(value)
    positives = 0
    for fixture, contract in [('activity-configuration.json','configuration.schema.json'),('operation-ensure-power.json','wire-envelope.schema.json'),('operation-press-key.json','wire-envelope.schema.json'),('owner-state.json','wire-envelope.schema.json')]:
        value=read('examples/'+fixture);require(valid(contract,value),'Invalid fixture '+fixture);positives+=1
    for fixture,contract in [('qualified-device-names.json','qualified-device-name.schema.json'),('qualified-addresses.json','cec-address.schema.json')]:
        for item in read('examples/'+fixture):require(valid(contract,item),'Invalid '+fixture);positives+=1
    config=read('examples/activity-configuration.json')
    selector_count=validate_configuration(config)
    changed=read('examples/name-reresolution.json')
    require(valid('qualified-device-name.schema.json',changed['selector']) and changed['before']['logical_address']!=changed['after']['logical_address'] and changed['saved_selection_changes'] is False and changed['held_recipient_changes'] is False,'Invalid LA refresh example')
    negatives=[]
    for field in ['enabled','route_id']:
        candidate=copy.deepcopy(config);candidate['activities'][0][field]=False if field=='enabled' else '1'*32;negatives.append(('configuration.schema.json',candidate))
    for field in ['domains','binding_policies','resources','signal_edges','routes']:
        candidate=copy.deepcopy(config);candidate[field]=[];negatives.append(('configuration.schema.json',candidate))
    candidate=copy.deepcopy(config);candidate['components'][0]['capabilities']=[];negatives.append(('configuration.schema.json',candidate))
    candidate=copy.deepcopy(config);candidate['version']=2;negatives.append(('configuration.schema.json',candidate))
    candidate=copy.deepcopy(config);candidate['components'][0]['qualified_names'].append(copy.deepcopy(candidate['components'][1]['qualified_names'][0]));negatives.append(('configuration.schema.json',candidate))
    name=copy.deepcopy(changed['selector'])
    for field,value in [('logical_address',4),('tentacle_id',maximum),('side','both'),('osd_name','')]:
        candidate=copy.deepcopy(name);candidate[field]=value;negatives.append(('qualified-device-name.schema.json',candidate))
    power=read('examples/operation-ensure-power.json');key=read('examples/operation-press-key.json')
    synchronization=copy.deepcopy(power);synchronization['kind']='synchronization'
    synchronization['payload']={'round_id':'c'*32,'phase':'begin','config_revision':'8'}
    for phase in ['begin','ack','reject']:
        candidate=copy.deepcopy(synchronization);candidate['payload']['phase']=phase
        require(valid('wire-envelope.schema.json',candidate),'Invalid same-stream synchronization barrier');positives+=1
    for field,value in [('round_id','0'*32),('round_id','c'*31),('phase','retry'),('config_revision','0'),('config_revision','08'),('config_revision','18446744073709551616'),('config_revision',8),('unexpected',True)]:
        candidate=copy.deepcopy(synchronization);candidate['payload'][field]=value;negatives.append(('wire-envelope.schema.json',candidate))
    for retired in ['relay_reserve','relay_record','relay_close','relay_result','operation','hold_release','hold_renew','binding_update','binding_result','plan_prepare','plan_barrier','plan_abort','plan_result','config_prepare','config_activate','config_result','operation_result','scan_request','scan_result']:
        candidate=copy.deepcopy(power);candidate['kind']=retired;negatives.append(('wire-envelope.schema.json',candidate))
    for value in ['0','01','18446744073709551616',1,True]:
        candidate=copy.deepcopy(power);candidate['payload']['sequence']=value;negatives.append(('wire-envelope.schema.json',candidate))
    for field,value in [('hold_lease_ms',750),('key',256),('repeat',1)]:
        candidate=copy.deepcopy(key);candidate['payload']['arguments'][field]=value;negatives.append(('wire-envelope.schema.json',candidate))
    snapshot=read('examples/owner-state.json')
    for field,value in [('adapter',maximum),('volume',128),('ready',1)]:
        candidate=copy.deepcopy(snapshot);candidate['payload']['snapshot'][field]=value;negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(snapshot);candidate['payload']['snapshot']['devices'][0]['logical_address']=15;negatives.append(('wire-envelope.schema.json',candidate))
    result=copy.deepcopy(snapshot);result['payload']={'input_result':{'generation':'1','sequence':'2','source_present':False}}
    require(valid('wire-envelope.schema.json',result),'Invalid owner source-presence result');positives+=1
    candidate=copy.deepcopy(result);candidate['payload']['input_result']['sequence']='0';negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(result);candidate['payload']['snapshot']=copy.deepcopy(snapshot['payload']['snapshot']);negatives.append(('wire-envelope.schema.json',candidate))
    context=copy.deepcopy(power);context['payload']['action']='context';context['payload']['target']['osd_name']=''
    context['payload']['arguments']={'selected':True,'source_changed':True,'source':copy.deepcopy(name),'audio':None,'display':None,'switch_device':None,'input':6}
    require(valid('wire-envelope.schema.json',context),'Invalid original activity context');positives+=1
    for field in ['display','switch_device','input']:
        candidate=copy.deepcopy(context);del candidate['payload']['arguments'][field];negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(config);del candidate['controller_octopus_id'];negatives.append(('configuration.schema.json',candidate))
    for value in [-1,4294967296,True,'0']:
        candidate=copy.deepcopy(power);candidate['payload']['setup_generation']=value;negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(snapshot);del candidate['payload']['snapshot']['setup_pending'];negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(snapshot);candidate['payload']['snapshot']['setup_pending']=65;negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(snapshot);del candidate['payload']['snapshot']['setup_replies_pending'];negatives.append(('wire-envelope.schema.json',candidate))
    candidate=copy.deepcopy(snapshot);candidate['payload']['snapshot']['setup_replies_pending']=1;negatives.append(('wire-envelope.schema.json',candidate))
    for contract,candidate in negatives:require(not valid(contract,candidate),'Accepted obsolete/invalid '+contract)
    wire=read('schemas/wire-envelope.schema.json')
    source=(ROOT.parent/'components/consortium/include/amity/consortium/wire.hpp').read_text()
    kinds=re.search(r'enum class Kind\s*\{([^}]+)\}',source).group(1)
    require(set(re.findall(r'(\w+)\s*=\s*[0-9]+',kinds))==set(wire['properties']['kind']['enum']),'Design kind registry differs from implementation')
    require(re.search(r'\bqualified_lookup\s*=\s*262144u\b',source),'Missing qualified-lookup capability')
    require(re.search(r'\blive_configuration\s*=\s*524288u\b',source),'Missing live configuration capability')
    hello_validator = Draft202012Validator({'$ref': production['$id']+'#/$defs/Hello'}, registry=registry)
    hello = dict(type='hello', protocol_min=1, protocol_max=1, consortium_id='1'*32,
                 octopus_id='2'*32, boot_id='3'*32, coordinator_epoch='1', config_revision='7',
                 config_hash='a'*64, firmware_identity='current', required_capabilities=0,
                 optional_capabilities=0, frame_cap=8192)
    for capabilities in (401472, 524288, 262144 | 524288, 1040970):
        candidate = dict(hello, required_capabilities=capabilities)
        require(hello_validator.is_valid(candidate),'Schema rejects current configuration capabilities')
    for capabilities in (4096, 1048576, 2147483648):
        candidate = dict(hello, required_capabilities=capabilities)
        require(not hello_validator.is_valid(candidate),'Schema accepts an unsupported required capability')
    manifest=read('MANIFEST.json');paths=set()
    for entry in manifest['files']:
        name=entry['path'];require(name not in paths and name!='MANIFEST.json','Duplicate manifest path');paths.add(name);data=(ROOT/name).read_bytes()
        require(len(data)==entry['size_bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256'],'Stale manifest '+name)
    actual={str(p.relative_to(ROOT)) for p in ROOT.rglob('*') if p.is_file() and p.suffix in ['.md','.json','.py'] and p.name!='MANIFEST.json'}
    require(actual==paths,'Manifest coverage differs from package')
    links=0;docs=list(ROOT.rglob('*.md'))
    for path in docs:
        for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)',path.read_text()):
            target=target.split(' "',1)[0]
            if re.match(r'^[A-Za-z][A-Za-z0-9+.-]*:',target) or target.startswith('#'):continue
            clean=unquote(target.split('#',1)[0]);require((path.parent/clean).exists(),'Missing link '+target+' in '+path.name);links+=1
    print(json.dumps({'manifest_entries_checked':len(paths),'schemas_checked':len(schemas),'positive_schema_vectors':positives,'negative_schema_vectors':len(negatives),'configured_selectors_checked':selector_count,'current_wire_kinds_checked':len(wire['properties']['kind']['enum']),'markdown_files_checked':len(docs),'local_links_checked':links,'firmware_or_hardware_tests_run':False},indent=2))

if __name__=='__main__':
    main()
