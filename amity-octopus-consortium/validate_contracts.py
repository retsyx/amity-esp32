#!/usr/bin/env python3
"""Validate this proposal's local schemas, fixtures and documentation links.

Requires Python 3.10+; full validation also needs jsonschema 4.x and referencing.
The --hardware-capacity-only mode uses only the standard library.
No network or firmware access occurs.
The optional --reference-repo path checks pinned Amity citation paths only.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent
BASE = 'https://amity.invalid/design/v0.1/'
PIN = 'd50af8026938797fc0820d0107f4c4e064c2d2bc'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read(relative):
    return json.loads((ROOT / relative).read_text())


def port_key(port):
    return tuple(port[k] for k in ('consortium_id', 'octopus_id', 'tentacle_id', 'side'))


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
    fields = ('FRONT_GPIO', 'BACK_GPIO', 'PRESENCE_GPIO', 'PRESENT_HIGH',
              'PRESENCE_PULLUP', 'PRESENCE_PULLDOWN')
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-repo', type=Path)
    parser.add_argument('--hardware-capacity-only', action='store_true',
                        help='Check canonical hardware limits across schemas, Kconfig and release tooling only')
    args = parser.parse_args()
    schemas = [json.loads(path.read_text()) for path in sorted((ROOT / 'schemas').glob('*.schema.json'))]
    max_tentacles = check_hardware_capacity(schemas)
    last_tentacle_id = max_tentacles - 1
    if args.hardware_capacity_only:
        print(json.dumps({'schemas_checked': len(schemas),
                          'hardware_capacity_consistency_checked': True,
                          'max_tentacles': max_tentacles,
                          'max_cec_ports': 2 * max_tentacles}, indent=2))
        return
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
    for schema in schemas:
        Draft202012Validator.check_schema(schema)
    manifest = read('MANIFEST.json')
    manifest_paths = set()
    for entry in manifest['files']:
        relative = entry['path']
        require(relative not in manifest_paths and relative != 'MANIFEST.json', 'Duplicate/recursive manifest entry')
        manifest_paths.add(relative)
        data = (ROOT / relative).read_bytes()
        require(len(data) == entry['size_bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'],
                f'Stale manifest entry {relative}')
    actual_paths = {str(p.relative_to(ROOT)) for p in ROOT.rglob('*')
                    if p.is_file() and p.suffix in ('.md', '.json', '.py') and p.name != 'MANIFEST.json'}
    require(manifest_paths == actual_paths, 'Manifest does not match current package files')
    registry = Registry().with_resources((schema['$id'], Resource.from_contents(schema)) for schema in schemas)

    def validator(name):
        return Draft202012Validator({'$ref': BASE + name}, registry=registry)

    addr = validator('cec-address.schema.json')
    qualified_name = validator('qualified-device-name.schema.json')
    binding_validator = validator('component-binding.schema.json')
    envelope = validator('wire-envelope.schema.json')
    uint64 = Draft202012Validator({'$ref': BASE + 'core-types.schema.json#/$defs/UInt64String'}, registry=registry)
    positives = 0
    addresses = read('examples/qualified-addresses.json')
    names = read('examples/qualified-device-names.json')
    bindings = read('examples/component-bindings.json')
    reresolution = read('examples/name-reresolution.json')
    operations = [json.loads(path.read_text()) for path in sorted((ROOT / 'examples').glob('operation-*.json'))]
    for v, objects in ((addr, addresses), (qualified_name, names),
                       (binding_validator, bindings), (envelope, operations),
                       (binding_validator, [reresolution['before']['binding'], reresolution['after']['binding']]),
                       (envelope, [reresolution['queued_operation_before_change']])):
        for obj in objects:
            v.validate(obj)
            positives += 1
    for text in ('0', '1', '9007199254740993', '18446744073709551615'):
        uint64.validate(text)
        positives += 1

    # The highest supported connector remains valid for both locator contracts.
    for v, obj in ((addr, addresses[0]), (qualified_name, names[0])):
        candidate = copy.deepcopy(obj)
        candidate['tentacle_id'] = last_tentacle_id
        v.validate(candidate)
        positives += 1

    # Delimiters and case are data inside the structured OSD field, not URI syntax.
    for text in ('HDMI / % Box', 'Apple TV', 'apple tv', ' Apple TV '):
        candidate = copy.deepcopy(names[0])
        candidate['osd_name'] = text
        qualified_name.validate(candidate)
        require(json.loads(json.dumps(candidate)) == candidate, 'OSD text changed in JSON round trip')
        positives += 1

    # Device metadata carries an actual canonical hostname, never a display alias.
    device_status = copy.deepcopy(operations[0])
    device_status['kind'] = 'device_status'
    for hostname in ('a', '1', 'amity-bedroom', 'a-b', 'a' * 32):
        device_status['payload'] = {'hostname': hostname}
        envelope.validate(device_status)
        positives += 1

    # Per-adapter settings are independent preferences, never topology authority.
    settings_snapshot = validator('adapter-settings.schema.json')
    settings_write = Draft202012Validator({'$ref': BASE + 'adapter-settings.schema.json#/$defs/WriteRequest'}, registry=registry)
    settings_pa = Draft202012Validator({'$ref': BASE + 'adapter-settings.schema.json#/$defs/PhysicalAddress'}, registry=registry)
    settings = {'boot_id': '3' * 32, 'settings_revision': '1', 'adapters': [
        {'tentacle_id': i, 'name': f'HDMI adapter {i+1}', 'configured': i in (0,last_tentacle_id),
         'presence': 'present' if i == 0 else 'unknown', 'presence_generation': str(i+1),
         'default_front_physical_address': f'{i+1}.0.0.0',
         'observed_front_physical_address': '2.0.0.0' if i == 0 else None,
         'effective_front_physical_address': '2.0.0.0' if i == 0 else None} for i in range(max_tentacles)]}
    write = {'request_id': '4' * 32, 'expected_boot_id': settings['boot_id'],
             'expected_settings_revision': '1', 'confirm_restart': True,
             'changes': [{'tentacle_id': last_tentacle_id, 'expected_presence_generation': str(max_tentacles),
                          'default_front_physical_address': ''}]}
    for v, value in ((settings_snapshot, settings), (settings_write, write)):
        v.validate(value); positives += 1
    for value in ('0.0.0.0','1.0.0.0','A.B.C.0','f.e.d.c'):
        settings_pa.validate(value); positives += 1

    negatives = []
    settings_packet = copy.deepcopy(operations[0]); settings_packet['kind'] = 'settings_write'
    settings_packet['payload'] = {'request_id': write['request_id'], 'execution_lease_id': '5' * 32,
                                  'settings': {k: v for k, v in write.items() if k != 'request_id'}}
    envelope.validate(settings_packet); positives += 1
    settings_result = copy.deepcopy(settings_packet); settings_result['kind'] = 'settings_result'
    settings_result['payload'] = {'request_id': write['request_id'], 'operation': 'write',
        'stage': 'applied', 'error': '', 'snapshot': settings, 'rebooting': True}
    envelope.validate(settings_result); positives += 1
    for field, value in (('rebooting', False), ('error', 'arbitrary text'), ('snapshot', None)):
        candidate = copy.deepcopy(settings_result); candidate['payload'][field] = value
        negatives.append((envelope, candidate, f'invalid applied settings result {field}'))
    candidate = copy.deepcopy(settings_packet); candidate['payload']['settings']['request_id'] = write['request_id']
    negatives.append((envelope, candidate, 'request ID copied into closed settings write core'))
    # Removal is explicit boot-bound factory reset; legacy retirement does not
    # authorize destructive effects and a scheduled result is not a reboot proof.
    reset_packet = copy.deepcopy(operations[0]); reset_packet['kind'] = 'membership_event'
    reset_packet['payload'] = {'mutation_id': '6' * 32, 'proposal_digest': '7' * 64,
        'phase': 'factory_reset', 'membership_revision': '2',
        'expected_boot_id': reset_packet['destination_boot_id']}
    envelope.validate(reset_packet); positives += 1
    candidate = copy.deepcopy(reset_packet); candidate['payload']['membership_revision'] = '0'
    envelope.validate(candidate); positives += 1 # Shape only: the owner rejects revisions <= its current revision.
    reset_result = copy.deepcopy(reset_packet); reset_result['kind'] = 'membership_result'
    for field in ('octopus_id', 'boot_id'):
        reset_result['origin_' + field] = reset_packet['destination_' + field]
        reset_result['destination_' + field] = reset_packet['origin_' + field]
    reset_result['payload']['expected_boot_id'] = reset_packet['destination_boot_id']
    for outcome in ('reset_scheduled', 'rejected', 'unknown'):
        reset_result['payload']['result'] = outcome
        envelope.validate(reset_result); positives += 1
    for field, value in (('phase', 'retire'), ('phase', 'reset'),
                         ('membership_revision', '01'),
                         ('expected_boot_id', '0' * 32), ('mutation_id', '0' * 32),
                         ('proposal_digest', '7' * 63), ('confirm_reset', True)):
        candidate = copy.deepcopy(reset_packet); candidate['payload'][field] = value
        negatives.append((envelope, candidate, f'invalid removal reset {field}'))
    candidate = copy.deepcopy(reset_packet); candidate['ttl_ms'] = 2001
    negatives.append((envelope, candidate, 'removal reset exceeds short original deadline'))
    candidate = copy.deepcopy(reset_packet); del candidate['payload']['expected_boot_id']
    negatives.append((envelope, candidate, 'removal reset without original target boot'))
    candidate = copy.deepcopy(reset_result); candidate['payload']['result'] = 'retired'
    negatives.append((envelope, candidate, 'old retired result cannot claim factory reset'))
    candidate = copy.deepcopy(reset_packet); candidate['payload']['result'] = 'reset_scheduled'
    negatives.append((envelope, candidate, 'request cannot claim its own reset result'))
    for value in ('1.0.2.0','F.F.F.F','1.2.3','10.0.0.0',' 1.0.0.0','0.0.0.1'):
        negatives.append((settings_pa, value, f'invalid settings PA {value!r}'))
    for field, value in (('confirm_restart', False), ('expected_boot_id', '0' * 32),
                         ('expected_settings_revision', '01'), ('expected_settings_revision', '0'),
                         ('changes', []), ('location', 'Bedroom')):
        candidate = copy.deepcopy(write); candidate[field] = value
        negatives.append((settings_write, candidate, f'invalid settings write {field}'))
    for field, value in (('tentacle_id', max_tentacles), ('expected_presence_generation', '0'),
                         ('default_front_physical_address', '1.0.2.0'), ('physical_presence', True)):
        candidate = copy.deepcopy(write); candidate['changes'][0][field] = value
        negatives.append((settings_write, candidate, f'invalid settings change {field}'))
    candidate = copy.deepcopy(settings); candidate['adapters'][last_tentacle_id]['tentacle_id'] = max_tentacles
    negatives.append((settings_snapshot, candidate, 'connector index exceeds hardware capacity'))
    candidate = copy.deepcopy(settings); candidate['adapters'].append(copy.deepcopy(settings['adapters'][0]))
    negatives.append((settings_snapshot, candidate, 'settings snapshot exceeds hardware capacity'))
    candidate = copy.deepcopy(write); candidate['changes'] *= max_tentacles + 1
    negatives.append((settings_write, candidate, 'settings changes exceed hardware capacity'))
    candidate = copy.deepcopy(settings); candidate['adapters'][last_tentacle_id]['name'] = 'HDMI adapter 1'
    negatives.append((settings_snapshot, candidate, 'renumbered sparse adapter'))
    candidate = copy.deepcopy(settings); candidate['adapters'][1]['presence'] = 'present'
    negatives.append((settings_snapshot, candidate, 'unconfigured adapter claims presence'))
    for hostname in ('', 'AMITY', '-amity', 'amity-', 'amity room', 'amity.room',
                     'amity_room', 'café', 'amity\n', 'amity\x00', 'a' * 33, 1, None):
        candidate = copy.deepcopy(device_status)
        candidate['payload'] = {'hostname': hostname}
        negatives.append((envelope, candidate, f'noncanonical device hostname {hostname!r}'))
    for payload in ({}, {'location': 'Bedroom'}, {'hostname': 'amity', 'location': 'Bedroom'}):
        candidate = copy.deepcopy(device_status)
        candidate['payload'] = payload
        negatives.append((envelope, candidate, 'missing hostname or unsupported display alias'))
    for field, value in [('tentacle_id', max_tentacles), ('tentacle_id', -1), ('side', 'source'),
                         ('logical_address', 15), ('logical_address', '4'),
                         ('octopus_id', 'A' * 32), ('octopus_id', '0' * 32)]:
        candidate = copy.deepcopy(addresses[0])
        candidate[field] = value
        negatives.append((addr, candidate, f'address {field}={value}'))
    candidate = copy.deepcopy(addresses[0])
    del candidate['consortium_id']
    negatives.append((addr, candidate, 'missing consortium'))
    for field, value in [('osd_name', ''), ('osd_name', 4), ('osd_name', 'x' * 15),
                         ('logical_address', 4), ('tentacle_id', max_tentacles), ('side', 'source')]:
        candidate = copy.deepcopy(names[0])
        candidate[field] = value
        negatives.append((qualified_name, candidate, f'qualified name {field}={value!r}'))
    for field in ('osd_name', 'consortium_id'):
        candidate = copy.deepcopy(names[0])
        del candidate[field]
        negatives.append((qualified_name, candidate, f'qualified name missing {field}'))
    negatives.append((qualified_name, addresses[0], 'runtime address substituted for configured name'))
    negatives.append((addr, names[0], 'configured name substituted for runtime address'))
    for value in ('01', '-1', '18446744073709551616', '1.0', '1\n', '', 1, 1.0):
        negatives.append((uint64, value, f'noncanonical uint64 {value!r}'))
    for field, value in [('protocol_version', 2), ('ttl_ms', 10001), ('kind', 'raw_cec'),
                         ('unexpected', True)]:
        candidate = copy.deepcopy(operations[0])
        candidate[field] = value
        negatives.append((envelope, candidate, f'envelope {field}'))
    for removed_kind in ('ingress_lease', 'intent', 'remote_management', 'remote_management_result', 'handover'):
        candidate = copy.deepcopy(operations[0])
        candidate['kind'] = removed_kind
        negatives.append((envelope, candidate, f'removed protocol kind {removed_kind}'))
    power = next(op for op in operations if op['payload']['operation'] == 'ensure_power')
    for sequence in (None, '0'):
        candidate = copy.deepcopy(power)
        if sequence is None:
            del candidate['payload']['command_sequence']
        else:
            candidate['payload']['command_sequence'] = sequence
        negatives.append((envelope, candidate, f'missing/zero command sequence {sequence}'))
    candidate = copy.deepcopy(power)
    candidate['payload']['arguments'] = {'toggle': True}
    negatives.append((envelope, candidate, 'ensure_power toggle substitution'))
    candidate = copy.deepcopy(power)
    del candidate['payload']['execution_lease_id']
    negatives.append((envelope, candidate, 'missing execution lease'))
    key = next(op for op in operations if op['payload']['operation'] == 'key_press')
    candidate = copy.deepcopy(key)
    candidate['payload']['arguments']['hold_lease_ms'] = 2001
    negatives.append((envelope, candidate, 'retired per-key lease field'))
    proxy = next(b for b in bindings if b['binding_kind'] == 'proxy_representation')
    candidate = copy.deepcopy(proxy)
    del candidate['proxy_identity_resource_id']
    negatives.append((binding_validator, candidate, 'proxy missing identity resource'))
    candidate = copy.deepcopy(proxy)
    candidate['role'] = 'tv_controller'
    negatives.append((binding_validator, candidate, 'proxy with wrong local role'))
    candidate = copy.deepcopy(proxy)
    candidate['device_name'] = names[0]
    negatives.append((binding_validator, candidate, 'proxy mistaken for native name selector'))
    native = next(b for b in bindings if b['binding_kind'] == 'native_appliance')
    candidate = copy.deepcopy(native)
    del candidate['device_name']
    negatives.append((binding_validator, candidate, 'native binding missing configured name'))
    candidate = copy.deepcopy(native)
    candidate['device_name'] = candidate['endpoint']
    negatives.append((binding_validator, candidate, 'native selector replaced by LA endpoint'))
    for v, obj, reason in negatives:
        require(not v.is_valid(obj), f'Negative vector was accepted: {reason}')

    scenario = read('examples/tv-projector-system.json')
    components = {c['component_id']: c for c in scenario['components']}
    require(len(components) == len(scenario['components']), 'Duplicate ComponentId')
    configured_names = []
    qualified_keys = set()
    for component in components.values():
        require(component['qualified_names'], 'Configured CEC fixture component has no qualified name')
        for name in component['qualified_names']:
            qualified_name.validate(name)
            positives += 1
            key = port_key(name) + (name['osd_name'],)
            require(key not in qualified_keys, 'Ambiguous duplicate configured name in valid fixture')
            qualified_keys.add(key)
            configured_names.append(name)
    require(names == configured_names, 'Name fixture differs from persisted component selectors')
    apple_names = [name for name in names if name['osd_name'] == 'Apple TV']
    require(len(apple_names) == 2 and port_key(apple_names[0]) != port_key(apple_names[1]),
            'Duplicate-OSD fixture must distinguish attachment scope')
    nodes = {o['octopus_id']: set(o['populated_tentacles']) for o in scenario['octopuses']}
    require(len(nodes) == len(scenario['octopuses']), 'Duplicate OctopusId')
    expected_ports = set()
    for node, slots in nodes.items():
        require(1 <= len(slots) <= max_tentacles and
                all(0 <= s <= last_tentacle_id for s in slots), 'Invalid populated slots')
        for slot in slots:
            for side in ('front', 'back'):
                expected_ports.add((scenario['consortium_id'], node, slot, side))
    domains = {d['domain_id']: d for d in scenario['cec_domains']}
    actual_ports = []
    allocations = {}
    for domain in domains.values():
        attached = {port_key(p) for p in domain['attached_ports']}
        actual_ports.extend(attached)
        logical = set()
        for allocation in domain['identity_allocations']:
            require(port_key(allocation['port']) in attached, 'Allocated identity on unattached port')
            la = allocation['logical_address']
            require(la not in logical, 'Duplicate owned logical address in one domain')
            logical.add(la)
            allocations[(domain['domain_id'], port_key(allocation['port']), la)] = allocation['role']
    require(set(actual_ports) == expected_ports and len(actual_ports) == len(expected_ports),
            'Each populated CEC port must occur in exactly one fixture domain')
    evidence = {e['evidence_id']: e for e in scenario['physical_address_evidence']}
    by_binding = {b['binding_id']: b for b in bindings}
    require(len(by_binding) == len(bindings), 'Duplicate BindingId')
    native_identity = {}
    for binding in bindings:
        endpoint = binding['endpoint']
        require(binding['component_id'] in components, 'Binding refers to missing component')
        require(binding['owner_octopus_id'] == endpoint['octopus_id'], 'Binding owner mismatch')
        domain = domains[binding['domain_id']]
        require(port_key(endpoint) in {port_key(p) for p in domain['attached_ports']}, 'Binding domain mismatch')
        ev = evidence[binding['physical_address_evidence_ref']]
        require(port_key(endpoint) == port_key(ev['port']) and ev['domain_id'] == binding['domain_id'],
                'PA evidence applies to another attachment/domain')
        require(any(a['role'] == binding['role'] and port_key(a['port']) == port_key(endpoint)
                    for a in domain['identity_allocations']), 'Binding role not allocated on its local port')
        key = (binding['domain_id'], port_key(endpoint), endpoint['logical_address'])
        if binding['binding_kind'] == 'proxy_representation':
            require(allocations.get(key) == 'playback_proxy', 'Proxy LA is not locally allocated')
            require('device_name' not in binding, 'Proxy has a native device selector')
        else:
            name = binding['device_name']
            require(name in components[binding['component_id']]['qualified_names'],
                    'Native binding name is not configured on its component')
            require(port_key(name) == port_key(endpoint), 'Native name/endpoint attachment mismatch')
            native_key = (binding['domain_id'], endpoint['logical_address'])
            previous = native_identity.setdefault(native_key, binding['component_id'])
            require(previous == binding['component_id'], 'Native domain/LA aliases different components')
            require(all(a['logical_address'] != endpoint['logical_address'] for a in domain['identity_allocations']),
                    'Local identity collides with native appliance')
    for operation in operations:
        payload = operation['payload']
        binding = by_binding[payload['binding_id']]
        require(payload['target'] == binding['endpoint'], 'Operation target/binding mismatch')
        require(operation['destination_octopus_id'] == binding['owner_octopus_id'], 'Operation destination mismatch')
        require(operation['origin_octopus_id'] == scenario['coordinator_octopus_id'], 'Wrong operation authority')
        require(operation['consortium_id'] == scenario['consortium_id'], 'Wrong consortium')
        for name in ('component_id', 'binding_revision', 'domain_id', 'domain_revision'):
            require(payload[name] == binding[name], f'Operation {name} mismatch')
        require(operation['config_revision'] == scenario['config_revision'], 'Operation config mismatch')
        required = {'ensure_power': 'power', 'announce_active_source': 'proxy_activation', 'key_press': 'navigation'}
        require(binding['capability'] == required[payload['operation']], 'Operation capability mismatch')
    wired = {(link['octopus_id'], link['tentacle_id']) for link in scenario['physical_hdmi_links']}
    require(len(wired) == len(scenario['physical_hdmi_links']), 'A tentacle carries two fixture cables')
    require(wired == {(n, s) for n, slots in nodes.items() for s in slots}, 'Cable/slot mismatch')

    # The resolution fixture is a contract example, not an implementation of a live resolver.
    before, after = reresolution['before'], reresolution['after']
    old, new = before['binding'], after['binding']
    configured = reresolution['configured_component']
    require(configured == components[old['component_id']], 'Resolution fixture configured component mismatch')
    require(old['component_id'] == new['component_id'] == reresolution['activity_component_id'],
            'LA churn changed component or activity reference')
    require(old['device_name'] == new['device_name'] == configured['qualified_names'][0],
            'LA churn rewrote persisted qualified name')
    require(before['config_revision'] == after['config_revision'] == scenario['config_revision'],
            'LA-only resolution changed config revision')
    require(old['binding_id'] == new['binding_id'] and
            int(new['binding_revision']) > int(old['binding_revision']), 'Resolution did not advance same binding')
    require(port_key(old['endpoint']) == port_key(new['endpoint']) == port_key(old['device_name']),
            'LA-only resolution changed attachment')
    require(old['endpoint']['logical_address'] == 4 and new['endpoint']['logical_address'] == 8,
            'LA-churn fixture no longer demonstrates 4 to 8')
    observations = reresolution['fresh_observations']
    for observation in observations:
        addr.validate(observation['endpoint'])
        positives += 1
    matches = [o for o in observations if o['kind'] == 'native_appliance'
               and port_key(o['endpoint']) == port_key(new['device_name'])
               and o['osd_name'] == new['device_name']['osd_name']]
    require(len(matches) == 1 and matches[0]['endpoint'] == new['endpoint'],
            'Resolution fixture must have exactly one matching fresh native observation')
    require(any(o['endpoint'] == old['endpoint'] and o['osd_name'] != old['device_name']['osd_name']
                for o in observations), 'Fixture must show old LA reused by another name')
    queued = reresolution['queued_operation_before_change']['payload']
    require(queued['target'] == old['endpoint'] and queued['binding_revision'] == old['binding_revision'],
            'Queued fixture was not admitted against old binding')
    require(queued['target'] != new['endpoint'] and queued['binding_revision'] != new['binding_revision'],
            'Old queued operation incorrectly matches updated resolution')
    policy = reresolution['expected_policy']
    require(all(policy[k] is False for k in ('user_confirmation_required', 'activity_edit_required',
                'select_or_resume_required_for_la_change_alone', 'old_held_input_retargeted',
                'discovery_emits_power_or_routing')), 'Resolution fixture enables forbidden side effect')
    require(all(policy[k] is True for k in ('coordinator_actuator_binding_sync_required',
                'all_derived_capability_bindings_updated_together', 'old_binding_work_rejected')),
            'Resolution fixture omits required binding fence or synchronization')

    # Check local targets and reference labels without interpreting remote content.
    local_links = 0
    repo_paths = set()
    docs = sorted(ROOT.rglob('*.md'))
    for path in docs:
        text = path.read_text()
        require(text.count('```') % 2 == 0, f'Unbalanced fenced blocks in {path.name}')
        refs = dict(re.findall(r'^\[([^\]]+)\]:\s+(\S+)', text, re.M))
        targets = re.findall(r'\[[^\]]*\]\(([^)]+)\)', text) + list(refs.values())
        for label in re.findall(r'\[[^\]]+\]\[([^\]]+)\]', text):
            require(label in refs, f'Unresolved reference label {label} in {path.name}')
        for target in targets:
            target = target.split(' "', 1)[0]
            if re.match(r'^[A-Za-z][A-Za-z0-9+.-]*:', target) or target.startswith('#'):
                continue
            clean = unquote(target.split('#', 1)[0])
            require((path.parent / clean).exists(), f'Missing local link {target} in {path.name}')
            local_links += 1
        repo_paths.update(re.findall(r'https://github\.com/retsyx/amity-esp32/(?:blob|tree)/' + PIN + r'/([^\s)#]+)', text))
    if args.reference_repo:
        for relative in repo_paths:
            require((args.reference_repo / relative).exists(), f'Missing pinned repository path {relative}')
    result = {'manifest_entries_checked': len(manifest_paths),
              'schemas_checked': len(schemas), 'hardware_capacity_consistency_checked': True,
              'positive_schema_vectors': positives,
              'negative_schema_vectors': len(negatives), 'binding_relationships_checked': len(bindings),
              'operation_relationships_checked': len(operations), 'fixture_cec_ports': len(expected_ports),
              'configured_qualified_names_checked': len(configured_names),
              'native_name_binding_relationships_checked': sum(b['binding_kind'] == 'native_appliance' for b in bindings),
              'la_reresolution_fixture_checked': True,
              'markdown_files_checked': len(docs), 'local_link_targets_checked': local_links,
              'pinned_repository_paths_checked': len(repo_paths) if args.reference_repo else 0,
              'firmware_or_hardware_tests_run': False}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'VALIDATION FAILED: {error}', file=sys.stderr)
        raise SystemExit(1)
