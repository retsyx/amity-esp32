#!/usr/bin/env python3
"""Check that a new host test executes every firmware line an old one did.

Both sets of tests run with clang source-based coverage: the old ones in a copy
of --old-ref, the new ones in a copy of the work tree or of --new-ref, both
under test-results/coverage. CTest executables come from a coverage build of
tests/. The Python tests compile their own binaries; a compiler wrapper adds
the coverage flags and keeps every generated source and linked binary. Clang
counts lines of the file it compiled, so a line of a generated source is
credited to the firmware line that its slice_support #line directive names.

For each firmware file under components/, main/ or third_party/ that the old
tests executed, or each --file, the report lists the executed lines the new
tests miss. Exit status: 0 when nothing is missed, 1 when a line is, and 2 when
the tests or tools fail or a compared file differs between the two copies.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_test_tree as tree_support  # noqa: E402
from host_test_tree import Failure, ROOT, tail  # noqa: E402

FIRMWARE = ('components/', 'main/', 'third_party/')
DIRECTIVE = re.compile(r'\s*#\s*(?:line\s+)?(\d+)(?:\s+"((?:\\.|[^"\\])*)")?')


def llvm_tools(directory):
    """Return the host clang, clang++, llvm-profdata and llvm-cov commands to use.

    The LLVM tools must match the compiler, so they come from beside clang, or
    from Xcode for Apple's clang, before PATH.
    """
    tools = {}
    for name in ('clang', 'clang++'):
        tools[name] = shutil.which(name, path=directory) if directory else tree_support.host_compiler(name)
    beside = Path(os.path.realpath(tools['clang'])).parent if tools['clang'] else None
    for name in ('llvm-profdata', 'llvm-cov'):
        found = shutil.which(name, path=directory or str(beside)) if directory or beside else None
        if not found and not directory and sys.platform == 'darwin':
            result = subprocess.run(['xcrun', '--find', name], capture_output=True, text=True)
            found = result.stdout.strip() if result.returncode == 0 else None
        tools[name] = found or (None if directory else shutil.which(name))
    missing = [name for name, found in tools.items() if not found]
    if missing:
        raise Failure(f'{", ".join(missing)} not found; install LLVM or pass --llvm-bin')
    return tools


def presumed_lines(text):
    """Map each physical line of a generated source to the (file, line) its #line directives give it."""
    mapping, file, line = {}, None, 1
    for number, content in enumerate(text.splitlines(), 1):
        directive = DIRECTIVE.match(content)
        if directive:
            line = int(directive.group(1))
            if directive.group(2) is not None:
                file = re.sub(r'\\(.)', r'\1', directive.group(2))
            continue
        mapping[number] = (file, line)
        line += 1
    return mapping


def lcov_lines(text):
    """Yield (source file, executed line numbers) from llvm-cov's lcov export."""
    source, executed = None, set()
    for line in text.splitlines():
        if line.startswith('SF:'):
            source, executed = line[3:], set()
        elif line.startswith('DA:'):
            number, count = line[3:].split(',')[:2]
            if int(count) > 0:
                executed.add(int(number))
        elif line == 'end_of_record' and source:
            yield source, executed
            source = None


def binary_sources(records):
    """Return (kept binary, {generated source: kept copy}) for each binary the wrapper linked."""
    outputs = {}
    linked = []
    for record in records:
        if record['mode'] == 'compile' and 'output' in record:
            outputs[record['output']] = record['snapshot']
        elif record['mode'] == 'link':
            snapshot = {}
            for obj in record['objects']:
                snapshot.update(outputs.get(obj, {}))
            snapshot.update(record['snapshot'])
            linked.append((record['binary'], snapshot))
    return linked


def collect(label, ref, regex, tier, tools, warnings):
    """Run the matching tests with coverage; return (copy root, {firmware file: executed lines})."""
    base = ROOT / 'test-results/coverage' / label
    print(f'{label}: copying {ref or "the work tree"} to {base.relative_to(ROOT)}', flush=True)
    tree = tree_support.materialize(base / 'tree', ref)
    keep = (base / 'keep').resolve()
    if keep.exists():
        shutil.rmtree(keep)
    (keep / 'profiles').mkdir(parents=True)
    build = tree / 'test-results/coverage-build'
    flags = ' '.join(tree_support.COVERAGE_FLAGS)
    tree_support.configure(tree, build, tier, f'-DCMAKE_C_COMPILER={tools["clang"]}',
                           f'-DCMAKE_CXX_COMPILER={tools["clang++"]}', f'-DCMAKE_C_FLAGS={flags}',
                           f'-DCMAKE_CXX_FLAGS={flags}', '-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate')
    tests = tree_support.list_tests(build, regex)
    if not tests:
        raise Failure(f'{label}: no test matches {regex!r}')
    targets = tree_support.test_targets(build, tests)
    print(f'{label}: {len(tests)} tests, building {", ".join(targets) or "nothing"}', flush=True)
    tree_support.build_targets(build, targets)
    compilers = {name: tools['clang'] for name in ('cc', 'gcc', 'clang')}
    compilers.update({name: tools['clang++'] for name in ('c++', 'g++', 'clang++')})
    wrappers = tree_support.write_compiler_wrappers(base / 'wrappers', compilers)
    environment = tree_support.wrapper_environment(wrappers, AMITY_WRAPPER_COVERAGE=keep, AMITY_WRAPPER_TREE=tree,
                                                   LLVM_PROFILE_FILE=keep / 'profiles' / '%p-%m.profraw')
    passed, failed, _, output = tree_support.run_tests(build, regex, environment)
    if not passed:
        raise Failure(f'{label}: tests failed: {", ".join(failed)}\n{tail(output, 30)}')
    if (keep / 'wrapper-errors.log').exists():
        warnings.append(f'{label}: the compiler wrapper could not keep everything:\n'
                        + tail((keep / 'wrapper-errors.log').read_text(), 5))
    profiles = sorted((keep / 'profiles').glob('*.profraw'))
    if not profiles:
        raise Failure(f'{label}: the tests wrote no coverage profile')
    profile = keep / 'merged.profdata'
    tree_support.run([tools['llvm-profdata'], 'merge', '-sparse', *profiles, '-o', profile])
    binaries = sorted({(build / 'bin' / target).as_posix() for target in targets})
    linked = [(binary, {}) for binary in binaries]
    linked += binary_sources(tree_support.compile_records(keep))
    executed = {}
    for binary, snapshot in linked:
        exported = subprocess.run([tools['llvm-cov'], 'export', '-format=lcov', '-instr-profile', str(profile), binary],
                                  capture_output=True, text=True)
        if exported.returncode:
            raise Failure(f'{label}: llvm-cov export {binary} failed:\n{tail(exported.stderr, 10)}')
        for source, lines in lcov_lines(exported.stdout):
            path = Path(os.path.realpath(source))
            if str(path) in snapshot:
                text = Path(snapshot[str(path)]).read_text(errors='replace')
                mapping = presumed_lines(text)
                if lines and not any(file for file, _ in mapping.values()):
                    warnings.append(f'{label}: {path.name}, generated for {Path(binary).name.split("-", 2)[-1]}, has no '
                                    '#line directives; any slices in it are not credited to firmware lines')
                mapped = set()
                for line in lines:
                    file, presumed = mapping.get(line, (None, 0))
                    if file and Path(os.path.realpath(file)).is_relative_to(tree):
                        mapped.add((Path(os.path.realpath(file)).relative_to(tree).as_posix(), presumed))
            elif path.is_relative_to(tree):
                mapped = {(path.relative_to(tree).as_posix(), line) for line in lines}
            else:
                continue
            for file, line in mapped:
                if file.startswith(FIRMWARE):
                    executed.setdefault(file, set()).add(line)
    return tree, executed


def ranges(lines):
    """Return sorted line numbers as compact ranges, such as 3-7, 10."""
    parts, numbers = [], sorted(lines)
    start = previous = None
    for number in numbers + [None]:
        if number is not None and previous is not None and number == previous + 1:
            previous = number
            continue
        if start is not None:
            parts.append(str(start) if start == previous else f'{start}-{previous}')
        start = previous = number
    return ', '.join(parts)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--old', required=True, metavar='REGEX', help='CTest regular expression for the old tests')
    parser.add_argument('--old-ref', default='HEAD', help='git ref that holds the old tests (default: HEAD)')
    parser.add_argument('--new', required=True, metavar='REGEX', help='CTest regular expression for the new tests')
    parser.add_argument('--new-ref', help='git ref that holds the new tests (default: the work tree)')
    parser.add_argument('--file', action='append', default=[], help='compare only this firmware file; repeatable')
    parser.add_argument('--tier', choices=('all', 'standalone', 'sdk'), help='host tests to define (default: all when '
                        'the SDK-built inputs exist, otherwise standalone)')
    parser.add_argument('--llvm-bin', help='directory holding clang, clang++, llvm-profdata and llvm-cov')
    parser.add_argument('--json', type=Path, help='write the executed and missed lines to this file')
    args = parser.parse_args(argv)
    warnings = []
    try:
        tools = llvm_tools(args.llvm_bin)
        tier = args.tier or tree_support.default_tier()
        old_tree, old = collect('old', args.old_ref, args.old, tier, tools, warnings)
        new_tree, new = collect('new', args.new_ref, args.new, tier, tools, warnings)
    except Failure as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    differ, missed = [], {}
    print()
    for file in args.file or sorted(old):
        old_path, new_path = old_tree / file, new_tree / file
        if not old_path.is_file() or not new_path.is_file() or old_path.read_bytes() != new_path.read_bytes():
            differ.append(file)
            print(f'{file}: differs between the two copies, so its lines cannot be compared')
            continue
        missing = old.get(file, set()) - new.get(file, set())
        if missing:
            missed[file] = sorted(missing)
        print(f'{file}: old {len(old.get(file, ()))} lines, new {len(new.get(file, ()))}, '
              + (f'missed {len(missing)}: {ranges(missing)}' if missing else 'none missed'))
    for warning in dict.fromkeys(warnings):
        print(f'warning: {warning}')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'old': {file: sorted(lines) for file, lines in old.items()},
                                         'new': {file: sorted(lines) for file, lines in new.items()},
                                         'missed': missed, 'differ': differ}, indent=2) + '\n')
    if differ:
        return 2
    return 1 if missed else 0


if __name__ == '__main__':
    sys.exit(main())
