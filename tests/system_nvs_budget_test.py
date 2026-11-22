#!/usr/bin/env python3
"""Exercise the pinned IDF NVS page allocator, not a byte-only estimate."""
import csv
import os
from pathlib import Path
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

# A modeled record the firmware no longer names is a stale model.
sources = ''.join(path.read_text(errors='replace') for path in (ROOT / 'components').rglob('*')
                  if path.suffix in ('.c', '.cpp', '.h', '.hpp'))
stale = [name for space, values in records for name in [space] + [key for key, _ in values]
         if '"' + name + '"' not in sources]
assert not stale, ('NVS model names records the firmware no longer uses', stale)


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
