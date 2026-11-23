#!/usr/bin/env python3
"""Run host tests in a disposable copy of the repository.

check_test_mutants.py and compare_test_coverage.py edit or instrument a copy
under test-results/, never the checkout. A copy holds the work tree's tracked
and untracked files, or a git ref's files, with managed_components and build/
linked to the checkout's own.

The Python tests compile C++ themselves, so the copy's tests run with cc and
c++ wrappers in front of PATH and in CC and CXX. A wrapper runs the real
compiler and, as the environment asks, records failed compilations or adds
clang coverage flags and keeps each generated source and linked binary. Run as
a script, this module is that wrapper: host_test_tree.py <compiler> <args>.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CODE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inc', '.ipp'}
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx'}
COVERAGE_FLAGS = ['-fprofile-instr-generate', '-fcoverage-mapping', '-Wno-unused-command-line-argument']
# Compiler options whose value is the next argument.
VALUE_OPTIONS = {'-o', '-I', '-D', '-U', '-include', '-imacros', '-isystem', '-iquote', '-idirafter', '-isysroot',
                 '-MF', '-MT', '-MQ', '-x', '-arch', '-target', '-Xlinker', '-Xclang', '-Xpreprocessor', '-L',
                 '-framework', '-u', '-e'}
# A generated source's directory is kept whole unless it holds more files than this.
SNAPSHOT_LIMIT = 400


class Failure(Exception):
    """A step that leaves no meaningful result, such as a build that fails before any mutant."""


def run(command, **options):
    """Run command, returning its completed process; raise Failure with its output if it fails."""
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, **options)
    if result.returncode:
        raise Failure(f'{shlex.join(map(str, command))} failed:\n{tail(result.stdout + result.stderr)}')
    return result


def tail(text, lines=40):
    return '\n'.join(text.rstrip().splitlines()[-lines:])


def materialize(destination, ref=None):
    """Replace destination with a copy of the work tree, or of ref, and return its resolved path."""
    destination = Path(destination)
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    if ref:
        run(['git', '-C', ROOT, 'rev-parse', '--verify', '--quiet', f'{ref}^{{commit}}'])
        archive = subprocess.Popen(['git', '-C', str(ROOT), 'archive', '--format=tar', ref], stdout=subprocess.PIPE)
        extracted = subprocess.run(['tar', '-x', '-C', str(destination)], stdin=archive.stdout)
        archive.stdout.close()
        if archive.wait() or extracted.returncode:
            raise Failure(f'cannot extract {ref}')
    else:
        listed = run(['git', '-C', ROOT, 'ls-files', '-co', '--exclude-standard', '-z']).stdout
        for name in sorted(set(filter(None, listed.split('\0')))):
            source, target = ROOT / name, destination / name
            if not source.is_symlink() and not source.is_file():
                continue  # Deleted but still tracked.
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_symlink():
                target.symlink_to(os.readlink(source))
            else:
                shutil.copy2(source, target)
    for name in ('managed_components', 'build'):
        if (ROOT / name).exists() and not (destination / name).exists():
            (destination / name).symlink_to(ROOT / name, target_is_directory=True)
    return destination.resolve()


def default_tier():
    """Define every host test when the SDK-built inputs exist, otherwise only the standalone ones."""
    cjson = ROOT / 'managed_components/espressif__cjson'
    return 'all' if cjson.exists() else 'standalone'


def configure(tree, build, tier, *options, environment=None):
    run(['cmake', '-S', tree / 'tests', '-B', build, '-G', 'Ninja', f'-DAMITY_HOST_TESTS={tier}',
         f'-DPYTHON3={sys.executable}', *options], env=environment)


def list_tests(build, regex):
    """Return the names of the CTest tests that match the CTest regular expression."""
    listed = json.loads(run(['ctest', '--test-dir', build, '-R', regex, '--show-only=json-v1']).stdout)
    return [test['name'] for test in listed.get('tests', [])]


def test_commands(build):
    """Map each test to its command as the generated CTestTestfile.cmake files record it, before any build."""
    argument = re.compile(r'"((?:\\.|[^"\\])*)"|\[(=*)\[(.*?)\]\2\]', re.S)
    commands = {}
    for testfile in Path(build).rglob('CTestTestfile.cmake'):
        for call in re.finditer(r'^add_test\((.*)\)\s*$', testfile.read_text(), re.M):
            values = [re.sub(r'\\(.)', r'\1', match.group(1)) if match.group(1) is not None else match.group(3)
                      for match in argument.finditer(call.group(1))]
            if values:
                commands[values[0]] = values[1:]
    return commands


def test_targets(build, names):
    """Return the CMake targets that build the executables these tests run."""
    binaries = (Path(build) / 'bin').resolve()
    commands = test_commands(build)
    return sorted({Path(commands[name][0]).name for name in names
                   if commands.get(name) and Path(commands[name][0]).resolve().parent == binaries})


def build_targets(build, targets, environment=None):
    if targets:
        run(['cmake', '--build', build, '--target', *targets], env=environment)


def run_tests(build, regex, environment, timeout=None, jobs=None):
    """Run the matching tests; return (passed, failed test names, timed out, output)."""
    command = ['ctest', '--test-dir', str(build), '-R', regex, '--output-on-failure',
               '--parallel', str(jobs or os.cpu_count() or 4)]
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        timed_out = True
    failed = re.findall(r'^\s*\d+ - (\S+) \(', output.split('The following tests FAILED:', 1)[-1], re.M) \
        if 'The following tests FAILED:' in output else []
    return process.returncode == 0 and not timed_out, failed, timed_out, output


def write_compiler_wrappers(directory, compilers):
    """Write one wrapper per name in compilers, a mapping from name to the real compiler command."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, real in compilers.items():
        wrapper = directory / name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, '-S', str(Path(__file__).resolve()), real])
                           + ' "$@"\n')
        wrapper.chmod(0o755)
    return directory


def wrapper_environment(directory, **settings):
    """Return os.environ with the wrappers in directory first in PATH, in CC and CXX, plus settings."""
    environment = dict(os.environ)
    environment['PATH'] = str(directory) + os.pathsep + environment.get('PATH', '')
    environment['CC'] = str(Path(directory) / 'cc')
    environment['CXX'] = str(Path(directory) / 'c++')
    environment.update({key: str(value) for key, value in settings.items()})
    return environment


def host_compiler(name, environment_variable=None):
    """Return the first name in PATH that builds for this computer, skipping the SDK's cross compilers.

    The ESP-IDF environment puts its own clang ahead of the host's.
    """
    if environment_variable and os.environ.get(environment_variable):
        return os.environ[environment_variable]
    for directory in os.environ.get('PATH', '').split(os.pathsep):
        candidate = Path(directory or '.') / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            machine = subprocess.run([str(candidate), '-dumpmachine'], capture_output=True, text=True).stdout.strip()
            if machine and not machine.endswith('-elf') and '-esp' not in machine:
                return str(candidate)
    return None


def find_compiler(name, environment_variable=None):
    """Return the host compiler a test runs for name, or raise Failure when there is none."""
    found = host_compiler(name, environment_variable)
    if not found:
        raise Failure(f'no host {name} in PATH')
    return found


def compile_records(keep):
    """Return the coverage wrapper's records, oldest first."""
    log = Path(keep) / 'compiles.jsonl'
    if not log.exists():
        return []
    records = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
    return sorted(records, key=lambda record: record['time'])


def _arguments(args):
    """Split compiler arguments into (positional inputs, output or None, mode)."""
    inputs, output, mode, skip = [], None, 'link', False
    for index, argument in enumerate(args):
        if skip:
            skip = False
            continue
        if argument in VALUE_OPTIONS:
            skip = True
            if argument == '-o' and index + 1 < len(args):
                output = args[index + 1]
        elif argument.startswith('-o') and len(argument) > 2:
            output = argument[2:]
        elif argument == '-c':
            mode = 'compile'
        elif argument in ('-E', '-S', '-fsyntax-only', '-M', '-MM') and mode != 'compile':
            mode = 'other'
        elif not argument.startswith('-'):
            inputs.append(argument)
    return inputs, output, mode


def _snapshot(directory):
    """Return the code files under directory, or None when there are more than SNAPSHOT_LIMIT."""
    found = []
    for parent, directories, files in os.walk(directory):
        directories[:] = [name for name in directories if name not in ('CMakeFiles', '.git')]
        for name in files:
            if Path(name).suffix in CODE_SUFFIXES:
                found.append(Path(parent) / name)
                if len(found) > SNAPSHOT_LIMIT:
                    return None
    return found


def _keep(keep, args):
    """Keep the generated sources and linked binary of a successful coverage compilation."""
    cwd = Path.cwd()
    tree = Path(os.environ['AMITY_WRAPPER_TREE'])
    stamp = f'{time.time_ns()}-{os.getpid()}'
    inputs, output, mode = _arguments(args)
    if mode == 'other':
        return
    temporary = Path(tempfile.gettempdir()).resolve()
    snapshot = {}
    for source in (cwd / name for name in inputs if Path(name).suffix in SOURCE_SUFFIXES):
        source = source.resolve()
        # Python tests write their sources to a temporary directory or under test-results/.
        generated = source.is_relative_to(tree / 'test-results') or (
            source.is_relative_to(temporary) and not source.is_relative_to(ROOT))
        if not generated or source.is_relative_to(keep) or str(source) in snapshot:
            continue
        files = _snapshot(source.parent) or [source]
        for path in files:
            copy = keep / 'sources' / stamp / path.relative_to(path.anchor)
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, copy)
            snapshot[str(path)] = str(copy)
    record = {'time': time.time_ns(), 'mode': mode, 'snapshot': snapshot,
              'objects': [str((cwd / name).resolve()) for name in inputs if Path(name).suffix in ('.o', '.obj')]}
    if mode == 'compile':
        sources = [name for name in inputs if Path(name).suffix in SOURCE_SUFFIXES]
        if output or len(sources) == 1:
            record['output'] = str((cwd / (output or Path(sources[0]).with_suffix('.o').name)).resolve())
    elif mode == 'link':
        binary = (cwd / (output or 'a.out')).resolve()
        copy = keep / 'binaries' / f'{stamp}-{binary.name}'
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, copy)
        record.update(output=str(binary), binary=str(copy))
    with open(keep / 'compiles.jsonl', 'a') as log:
        log.write(json.dumps(record) + '\n')


def compile_main(real, args):
    """Run the real compiler as the wrapper's environment asks and return its status."""
    coverage = os.environ.get('AMITY_WRAPPER_COVERAGE')
    failures = os.environ.get('AMITY_WRAPPER_FAILURES')
    command = shlex.split(real) + (COVERAGE_FLAGS if coverage else []) + args
    result = subprocess.run(command, stderr=subprocess.PIPE if failures else None)
    status = result.returncode
    if failures:
        sys.stderr.buffer.write(result.stderr)
        sys.stderr.flush()
    # CMake's compiler checks fail on purpose inside CMakeFiles.
    if status and failures and '/CMakeFiles/' not in os.getcwd() + ' '.join(args):
        diagnostics = [line for line in result.stderr.decode(errors='replace').splitlines()
                       if re.search(r'(^|[\s:])error:', line)]
        with open(failures, 'a') as log:
            log.write(json.dumps({'cwd': os.getcwd(), 'command': command, 'errors': diagnostics[:3]}) + '\n')
    if not status and coverage:
        try:
            _keep(Path(coverage), args)
        except OSError as error:  # Reported by the coverage tool; the compilation itself succeeded.
            with open(Path(coverage) / 'wrapper-errors.log', 'a') as log:
                log.write(f'{shlex.join(command)}: {error}\n')
    return status


if __name__ == '__main__':
    sys.exit(compile_main(sys.argv[1], sys.argv[2:]))
