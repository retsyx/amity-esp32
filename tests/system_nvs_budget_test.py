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
    shared = [('amity_group', [('state', 6144), ('identity', 2048), ('identity_tmp', 2048)]),
              ('amity_origin', [('bootstrap', 833)]),
              ('amity', [('config', 8192), ('aliases', 4096), ('ota_job', 2048), ('ota_cohort', 9216)])]
    # Normal commits retain active + candidate until the exact marker/readback
    # decision. No obsolete setup draft or handover proposal is reserved.
    ordinary = [('amity_config', [('c0', 16384 + 256), ('c1', 16384 + 256), ('legacy_backup', 8192), ('intent0', 2048), ('intent1', 2048), ('metadata', 2048)])] + shared
    generate(directory, 'ordinary-atomic16k', ordinary, True)
    enrollment = [(space, list(values)) for space, values in ordinary]
    # Candidate enrollment records both old and staged WiFi settings, including
    # both atomic copies, until exact authenticated enrollment commits/aborts.
    enrollment[0][1].extend([('network_old', 3072), ('network_new', 3072)])
    generate(directory, 'candidate-network-decision', enrollment, True)
    pressure = [(space, list(values)) for space, values in enrollment]
    pressure[0][1].append(('extra', 32768))
    generate(directory, 'over-capacity-rejected', pressure, False)
    legacy = [('amity_config', [('c0', 32768), ('c1', 32768), ('legacy_backup', 8192)])] + shared
    generate(directory, 'old32k-three-copy-profile-rejected', legacy, False)
print('NVS native allocator: atomic16KiB configuration and candidate-network rollback fit112KiB; excess pressure rejects without deleting originals')
