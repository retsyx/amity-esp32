#!/usr/bin/env python3
"""Validate this proposal's local schemas, fixtures and documentation links.

Requires Python 3.10+ and jsonschema 4.x. No network or firmware access occurs.
The optional --reference-repo path checks pinned Amity citation paths only.
"""
import argparse
import copy
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-repo', type=Path)
    args = parser.parse_args()
    schemas = [json.loads(path.read_text()) for path in sorted((ROOT / 'schemas').glob('*.schema.json'))]
    for schema in schemas:
        Draft202012Validator.check_schema(schema)
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

    # Delimiters and case are data inside the structured OSD field, not URI syntax.
    for text in ('HDMI / % Box', 'Apple TV', 'apple tv', ' Apple TV '):
        candidate = copy.deepcopy(names[0])
        candidate['osd_name'] = text
        qualified_name.validate(candidate)
        require(json.loads(json.dumps(candidate)) == candidate, 'OSD text changed in JSON round trip')
        positives += 1

    negatives = []
    for field, value in [('tentacle_id', 5), ('tentacle_id', -1), ('side', 'source'),
                         ('logical_address', 15), ('logical_address', '4'),
                         ('octopus_id', 'A' * 32), ('octopus_id', '0' * 32)]:
        candidate = copy.deepcopy(addresses[0])
        candidate[field] = value
        negatives.append((addr, candidate, f'address {field}={value}'))
    candidate = copy.deepcopy(addresses[0])
    del candidate['consortium_id']
    negatives.append((addr, candidate, 'missing consortium'))
    for field, value in [('osd_name', ''), ('osd_name', 4), ('osd_name', 'x' * 15),
                         ('logical_address', 4), ('tentacle_id', 5), ('side', 'source')]:
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
    power = next(op for op in operations if op['payload']['operation'] == 'ensure_power')
    candidate = copy.deepcopy(power)
    candidate['payload']['arguments'] = {'toggle': True}
    negatives.append((envelope, candidate, 'ensure_power toggle substitution'))
    candidate = copy.deepcopy(power)
    del candidate['payload']['execution_lease_id']
    negatives.append((envelope, candidate, 'missing execution lease'))
    key = next(op for op in operations if op['payload']['operation'] == 'key_press')
    candidate = copy.deepcopy(key)
    candidate['payload']['arguments']['hold_lease_ms'] = 2001
    negatives.append((envelope, candidate, 'excessive hold lease'))
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
        require(1 <= len(slots) <= 5 and all(0 <= s <= 4 for s in slots), 'Invalid populated slots')
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
    result = {'schemas_checked': len(schemas), 'positive_schema_vectors': positives,
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
