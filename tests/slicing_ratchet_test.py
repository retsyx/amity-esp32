#!/usr/bin/env python3
"""Keep the list of Python tests that read firmware sources current.

tests/slicing_allowlist.txt lists every Python test that reads firmware
sources and what happens to it: pending:<phase> until the host-test overhaul
replaces it with a C++ test that compiles whole files, or lint, sdk-slice or
whole-file when it stays. A test reads firmware sources when it names a C or
C++ source, header or build file under components/ or main/, a directory that
holds one, or anything in the vendored HomeKit SDK, or when a tests/ helper
module it imports does. An unlisted test, an entry for any other file and a
malformed entry fail.
"""
import ast
from functools import lru_cache
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
ALLOWLIST = TESTS / 'slicing_allowlist.txt'
CODE = {'.c', '.cc', '.cpp', '.h', '.hpp', '.inc'}
FORK = 'third_party/esp-homekit-sdk'
MARKER = re.compile(r'pending:[A-Z][A-Za-z0-9]*|lint|sdk-slice|whole-file')


@lru_cache(maxsize=None)
def firmware(name):
    """Whether name is a firmware source, header or build file, a directory holding one, or in the HomeKit SDK."""
    relative = name.removeprefix('./').strip('/')
    if relative not in ('components', 'main', 'third_party') and not relative.startswith(('components/', 'main/', FORK)):
        return False
    path = ROOT / relative
    if not path.exists():
        return False
    if relative == 'third_party' or relative.startswith(FORK):
        return True
    if path.is_file():
        return path.suffix in CODE or path.name == 'CMakeLists.txt' or path.name.startswith('Kconfig')
    return any(child.suffix in CODE for child in path.rglob('*'))


def references(tree):
    """Return the firmware paths a module names, as path strings or as joins onto the repository root."""
    roots = {target.id for node in ast.walk(tree) if isinstance(node, ast.Assign) and '__file__' in ast.unparse(node.value)
             for target in node.targets if isinstance(target, ast.Name)}
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            words = node.value.split()
            if words and '/' in words[0] and firmware(words[0]):
                found.add(words[0])
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div) and isinstance(node.left, ast.Name) \
                and node.left.id in roots and isinstance(node.right, ast.Constant) \
                and isinstance(node.right.value, str) and firmware(node.right.value):
            found.add(node.right.value)
    return found


def imports(tree):
    """Return the tests/ modules a module imports."""
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module)
            names.update(f'{node.module}.{alias.name}' for alias in node.names)
    found = set()
    for name in names:
        module = TESTS.joinpath(*name.split('.'))
        found.update(path for path in (module.with_suffix('.py'), module / '__init__.py') if path.is_file())
    return found


def readers():
    """Return {test file name: reasons} for each test, not helper module, that reads firmware sources."""
    modules = {}
    for path in sorted(TESTS.glob('*.py')) + sorted(TESTS.glob('*/*.py')):
        tree = ast.parse(path.read_text(), str(path))
        modules[path] = (references(tree), imports(tree))
    helpers = {module for _, imported in modules.values() for module in imported}

    def reasons(path, seen):
        named, imported = modules[path]
        found = set(named)
        for module in imported - seen:
            if module in modules and reasons(module, seen | {path}):
                found.add(f'via {module.relative_to(TESTS).as_posix()}')
        return found

    found = {}
    for path in modules:
        if path.parent == TESTS and path not in helpers and path.name != Path(__file__).name:
            why = reasons(path, frozenset())
            if why:
                found[path.name] = sorted(why)
    return found, helpers


def main():
    found, helpers = readers()
    entries, problems = {}, []
    for number, line in enumerate(ALLOWLIST.read_text().splitlines(), 1):
        fields = line.split('#', 1)[0].split()
        if not fields:
            continue
        if len(fields) != 2 or not MARKER.fullmatch(fields[1]):
            problems.append(f'{ALLOWLIST.name}:{number}: expected "<test>.py pending:<phase>|lint|sdk-slice|whole-file"')
        elif fields[0] in entries:
            problems.append(f'{ALLOWLIST.name}:{number}: {fields[0]} is listed twice')
        else:
            entries[fields[0]] = fields[1]
    for name, why in sorted(found.items()):
        if name not in entries:
            problems.append(f'{name} reads firmware sources ({", ".join(why[:3])}) but has no entry in {ALLOWLIST.name}. '
                            'Prefer a C++ test that compiles the whole firmware file.')
    for name in sorted(set(entries) - set(found)):
        path = TESTS / name
        reason = ('does not exist' if not path.is_file() else 'is a helper module' if path in helpers
                  else 'reads no firmware sources')
        problems.append(f'{ALLOWLIST.name} lists {name}, which {reason}; remove the entry')
    if problems:
        raise SystemExit('\n'.join(problems))
    counts = {marker: sum(1 for value in entries.values() if value.split(':')[0] == marker)
              for marker in ('pending', 'lint', 'sdk-slice', 'whole-file')}
    print(f'PASS slicing allowlist: {len(entries)} Python tests read firmware sources: '
          + ', '.join(f'{count} {marker}' for marker, count in counts.items()))


if __name__ == '__main__':
    main()
