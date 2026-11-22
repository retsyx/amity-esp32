#!/usr/bin/env python3
"""Exercise the pinned IDF NVS page allocator, not a byte-only estimate."""
import csv
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
candidates = []
if os.environ.get('IDF_PYTHON_ENV_PATH'):
    candidates.append(Path(os.environ['IDF_PYTHON_ENV_PATH']) / 'bin/python')
candidates += sorted((Path.home() / '.espressif/python_env').glob('idf6.1*_env/bin/python'))
python = next((str(path) for path in candidates if path.is_file()), None)
if not python:
    raise SystemExit('Pinned IDF6.1 Python environment is required for native NVS occupancy verification')

# Every amity_cfg record at the largest size its writer stores.
records = [('amity_config', [('c0', 16384 + 256), ('c1', 16384 + 256), ('cm0', 128), ('cm1', 128)]),
           ('amity_group', [('state', 8192), ('identity', 2048), ('hostnames', 8192)]),
           ('amity_origin', [('candidate', 48), ('committed', 48)]),
           ('amity_system', [('candidate', 48), ('committed', 48)]),
           ('amity_reset', [('pending', 1)]),
           ('amity', [('tls_cert', 1600), ('tls_key', 1600), ('admin_auth', 97), ('front_defaults', 256),
                      ('hostname', 128), ('hap_name', 64), ('hap_setup', 404)])]

# The model names exactly the records that the amity_cfg writers store.
components = ROOT / 'components'
sources = {str(path.relative_to(components)): path.read_text(errors='replace') for path in components.rglob('*')
           if path.suffix in ('.c', '.cpp', '.h', '.hpp', '.inc')}
writers = sorted(name for name, text in sources.items() if re.search(r'nvs_open_from_partition\(\s*"amity_cfg"', text))
assert writers == ['config_store/config_store.cpp', 'config_store/system_configuration_nvs.cpp',
                   'config_store/system_identity.cpp', 'consortium/credentials.cpp',
                   'device_settings/factory_reset.cpp'], ('Model the records of this amity_cfg writer', writers)
space = lambda name: re.search(r'nvs_open_from_partition\(\s*"amity_cfg"\s*,\s*"(\w+)"\s*,\s*NVS_READWRITE', sources[name]).group(1)
everything = ''.join(sources.values())
stored = {(space('config_store/config_store.cpp'), key) for key in re.findall(r'config_store::write\(\s*"(\w+)"', everything)}
stored |= set(re.findall(r'write_record\(\s*"(\w+)"\s*,\s*"(\w+)"', everything))
stored |= {(space('config_store/system_configuration_nvs.cpp'), key) for pair in
           re.findall(r'write_verified\(\s*\w+\s*\?\s*"(\w+)"\s*:\s*"(\w+)"', sources['config_store/configuration_journal.cpp']) for key in pair}
stored |= {(name, key) for name in re.findall(r'\(\s*"(\w+)"\s*\)', re.search(r'NvsIdentity\s+(\w+\(\s*"\w+"\s*\)[^;]*);', sources['config_store/system_identity.cpp']).group(1))
           for key in re.findall(r'store\.write\(\s*"(\w+)"', sources['config_store/include/identity_journal.hpp'])}
stored |= {(space('device_settings/factory_reset.cpp'), key) for key in
           re.findall(r'nvs_set_\w+\(\s*journal\.value\s*,\s*"(\w+)"', sources['device_settings/factory_reset.cpp'])}
modeled = {(name, key) for name, values in records for key, _ in values}
assert modeled == stored, ('NVS model differs from the stored records', sorted(modeled - stored), sorted(stored - modeled))


def generate(directory, name, records, fits):
    folder = directory / name
    folder.mkdir()
    rows = [['key', 'type', 'encoding', 'value']]
    for namespace, values in records:
        rows.append([namespace, 'namespace', '', ''])
        for key, size in values:
            blob = folder / (namespace + '_' + key + '.bin')
            blob.write_bytes(bytes((index * 73 + 19) & 255 for index in range(size)))
            rows.append([key, 'file', 'binary', str(blob)])
    source = folder / 'input.csv'
    with source.open('w', newline='') as stream:
        csv.writer(stream).writerows(rows)
    output = folder / 'partition.bin'
    result = subprocess.run([python, '-m', 'esp_idf_nvs_partition_gen', 'generate', str(source), str(output), '0x1c000'], capture_output=True, text=True)
    succeeded = result.returncode == 0 and output.exists() and output.stat().st_size == 0x1c000
    assert succeeded == fits, (name, result.stdout, result.stderr)


(ROOT / 'test-results').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='nvs-budget-', dir=ROOT / 'test-results') as temporary:
    directory = Path(temporary)
    generate(directory, 'all-records', records, True)
    pressure = [(space, list(values)) for space, values in records]
    pressure[0][1].append(('extra', 65536))
    generate(directory, 'over-capacity-rejected', pressure, False)
print('NVS native allocator: every amity_cfg record at its largest size fits 112KiB; excess pressure rejects')
