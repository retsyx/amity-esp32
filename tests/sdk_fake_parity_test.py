#!/usr/bin/env python3
"""Check that the shared SDK fakes keep the pinned SDK's constants and layouts.

Each tests/sdk_parity/<area>.def lists AMITY_PARITY(name, expression) entries.
Its header comments name the fake directories ("fakes:"), the headers that
declare the entries ("headers:") and the firmware source whose compile command
gives the target compiler and flags ("target:"). The entries are evaluated
against the fakes with the host compiler, and the same expressions are then
compiled against the real headers with the firmware's compiler and flags, each
asserted equal to its host value. Requires a firmware build.
"""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LISTS = ROOT / 'tests/sdk_parity'
COMMANDS = ROOT / 'build/compile_commands.json'
ENTRY = re.compile(r'^AMITY_PARITY\((\w+),', re.M)
# Compile-command arguments that name outputs; each takes the next argument.
OUTPUT_OPTIONS = {'-o', '-MF', '-MT', '-MQ'}
DEPENDENCY_FLAGS = {'-c', '-MD', '-MMD', '-MP'}


def metadata(definitions):
    """Return the fakes, headers and target source a .def names."""
    found = {}
    for key in ('fakes', 'headers', 'target'):
        match = re.search(rf'^// {key}: (.+)$', definitions, re.M)
        if not match:
            raise AssertionError(f'the list names no {key}')
        found[key] = match.group(1).split()
    if len(found['target']) != 1:
        raise AssertionError('the list names more than one target source')
    return found['fakes'], found['headers'], found['target'][0]


def host_values(path, fakes, headers, directory):
    """Evaluate the entries against the fakes and return {name: value}."""
    source = directory / f'{path.stem}_host.cpp'
    source.write_text(''.join(f'#include "{header}"\n' for header in headers) + r'''
#include <cstddef>
#include <cstdio>
#include <vector>
namespace {
struct Entry {
    const char *name;
    long long value;
};
std::vector<Entry> &entries() {
    static std::vector<Entry> list;
    return list;
}
struct Add {
    Add(const char *name, long long value) { entries().push_back({name, value}); }
};
}  // namespace
#define AMITY_PARITY(name, expression) const Add add_##name{#name, (long long)(expression)};
''' + f'#include "{path}"\n' + r'''
int main() {
    for (const Entry &entry : entries()) std::printf("%s %lld\n", entry.name, entry.value);
}
''')
    binary = directory / f'{path.stem}_host'
    includes = [f'-I{ROOT / fake}' for fake in fakes]
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', *includes,
                   str(source), '-o', str(binary)], check=True)
    output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
    return dict((name, int(value)) for name, value in (line.split() for line in output.splitlines()))


def target_command(source):
    """Return the firmware compile command for source, without its outputs, and its directory."""
    if not COMMANDS.exists():
        raise AssertionError('build/compile_commands.json is missing; build the firmware first')
    wanted = (ROOT / source).resolve()
    for entry in json.loads(COMMANDS.read_text()):
        if Path(entry['file']).resolve() != wanted:
            continue
        arguments = entry['arguments'] if 'arguments' in entry else shlex.split(entry['command'])
        kept, skip = [], False
        for argument in arguments:
            if skip:
                skip = False
            elif argument in OUTPUT_OPTIONS:
                skip = True
            elif argument not in DEPENDENCY_FLAGS and Path(entry['directory'], argument).resolve() != wanted:
                kept.append(argument)
        return kept, entry['directory']
    raise AssertionError(f'{source} is not in build/compile_commands.json')


def check(path, directory):
    definitions = path.read_text()
    names = ENTRY.findall(definitions)
    if not names or len(set(names)) != len(names):
        raise AssertionError(f'{path.name} must list each entry once')
    fakes, headers, source = metadata(definitions)
    values = host_values(path, fakes, headers, directory)
    if sorted(values) != sorted(names):
        raise AssertionError(f'{path.name}: the host evaluated {sorted(values)}, not the listed entries')
    generated = directory / f'{path.stem}_target.cpp'
    generated.write_text('#include <stddef.h>\n' + ''.join(f'#include "{header}"\n' for header in headers) +
                         '#define AMITY_PARITY(name, expression) static_assert((long long)(expression) == '
                         'AMITY_HOST_##name, "the fake differs from the SDK: " #name);\n' +
                         ''.join(f'#define AMITY_HOST_{name} ({value}LL)\n' for name, value in values.items()) +
                         f'#include "{path}"\n')
    command, working = target_command(source)
    result = subprocess.run(command + ['-fsyntax-only', '-fdiagnostics-color=never', '-x', 'c++', str(generated)], cwd=working,
                            capture_output=True, text=True)
    if result.returncode:
        problems = [line for line in result.stderr.splitlines() if 'error' in line] or result.stderr.splitlines()
        raise AssertionError(f'{path.name}: the fakes differ from the SDK headers:\n  ' + '\n  '.join(problems[:20]))
    return len(names)


def main():
    lists = sorted(LISTS.glob('*.def'))
    if not lists:
        print('tests/sdk_parity holds no lists', file=sys.stderr)
        return 1
    failed = False
    with tempfile.TemporaryDirectory(prefix='amity-sdk-parity-') as temporary:
        for path in lists:
            try:
                print(f'{path.name}: {check(path, Path(temporary))} entries match the SDK')
            except (AssertionError, subprocess.CalledProcessError) as error:
                print(error, file=sys.stderr)
                failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
