#!/usr/bin/env python3
"""Check that host tests fail when firmware code is broken on purpose.

A manifest, tests/mutants/<test>.json, names the CTest tests that guard some
firmware code and lists mutants, small edits to firmware sources:

    {"ctest": "^consortium_runtime_test\\.",
     "targets": ["consortium_runtime_test"],
     "baseline": {"ctest": "^consortium_runtime_status_test\\.py$"},
     "mutants": [{"id": "ttl-499", "check": "a request older than 500 ms is dropped",
                  "file": "components/consortium/runtime.cpp",
                  "find": "age >= 500", "replace": "age >= 499"}]}

ctest is a CTest regular expression. targets are the CMake targets to rebuild
after each edit; by default they are the executables the matching tests run.
baseline names the tests the manifest's tests replaced, for --baseline. A
mutant's find text must match exactly once in file once runs of whitespace are
treated as equal. A mutant marked "survives" with a reason is a known gap.

The tests run in a copy of the work tree, or of --ref, under
test-results/mutants. Each mutant is applied, the targets rebuilt and the tests
run, then the file is restored. A mutant is killed when a test fails, survives
when they all pass, and is an error when its find text does not match once or
the mutated code does not compile, including in a Python test's compiler call.

--baseline --ref <parent commit> --json old.json runs the mutants on the tests
being replaced, and --require-kills old.json then fails unless the new tests
kill every mutant those killed. --stubs TEST prints a manifest skeleton with an
early return at the top of each firmware function TEST slices: void returns,
bool returns false and true, esp_err_t returns ESP_FAIL and anything else
returns a value-initialized result.

Exit status: 0 when every mutant is killed, 1 when one survives or --require-kills
finds a mutant killed before that is no longer killed, and 2 on any error.
"""
import argparse
import ast
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_test_tree as tree_support  # noqa: E402
from host_test_tree import Failure, ROOT, tail  # noqa: E402

MANIFESTS = ROOT / 'tests/mutants'
FIRMWARE = ('components/', 'main/', 'third_party/')
CODE_SUFFIXES = {'.c', '.cc', '.cpp', '.h', '.hpp', '.inc'}
MANIFEST_KEYS = {'ctest', 'targets', 'baseline', 'mutants'}
MUTANT_KEYS = {'id', 'check', 'file', 'find', 'replace', 'survives'}
KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'alignof', 'decltype', 'static_assert',
            'defined', 'operator', 'new', 'delete', 'throw', 'case', 'else', 'do', 'alignas', 'noexcept'}
SPECIFIERS = {'static', 'inline', 'virtual', 'explicit', 'extern', 'friend', 'typename'}


class MutantError(Exception):
    pass


def load_manifest(path):
    manifest = json.loads(Path(path).read_text())
    problems = []
    if not isinstance(manifest, dict):
        raise MutantError(f'{path}: a manifest is a JSON object')
    problems += [f'unknown key {key!r}' for key in set(manifest) - MANIFEST_KEYS]
    if not isinstance(manifest.get('ctest'), str) or not manifest['ctest']:
        problems.append('ctest must name the tests')
    baseline = manifest.get('baseline', {'ctest': '-'})
    if not isinstance(baseline, dict) or not isinstance(baseline.get('ctest'), str) or set(baseline) - {'ctest', 'targets'}:
        problems.append('baseline is {"ctest": ..., "targets": [...]}')
    for spec in (manifest, baseline if isinstance(baseline, dict) else {}):
        targets = spec.get('targets', [])
        if not isinstance(targets, list) or not all(isinstance(target, str) for target in targets):
            problems.append('targets is a list of CMake target names')
    mutants = manifest.get('mutants')
    if not isinstance(mutants, list) or not mutants:
        problems.append('mutants must list at least one mutant')
        mutants = []
    seen = set()
    for index, mutant in enumerate(mutants):
        where = f'mutant {index + 1}'
        if not isinstance(mutant, dict):
            problems.append(f'{where} is not an object')
            continue
        where = f'mutant {mutant.get("id", index + 1)!r}'
        problems += [f'{where}: unknown key {key!r}' for key in set(mutant) - MUTANT_KEYS]
        missing = [key for key in ('id', 'check', 'file', 'find', 'replace') if not isinstance(mutant.get(key), str)]
        problems += [f'{where}: {key} must be a string' for key in missing]
        if missing:
            continue
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', mutant['id']):
            problems.append(f'{where}: id may use only letters, digits, ".", "_" and "-"')
        if mutant['id'] in seen:
            problems.append(f'{where}: repeated id')
        seen.add(mutant['id'])
        if Path(mutant['file']).is_absolute() or '..' in Path(mutant['file']).parts:
            problems.append(f'{where}: file is relative to the repository root')
        if not mutant['find'].split():
            problems.append(f'{where}: find is blank')
        if 'survives' in mutant and not (isinstance(mutant['survives'], str) and mutant['survives'].strip()):
            problems.append(f'{where}: survives gives the reason the tests cannot notice it')
    if problems:
        raise MutantError(f'{path}: ' + '; '.join(problems))
    return manifest


def mutate(text, mutant):
    """Return text with the mutant's find text, matched with runs of whitespace treated as equal, replaced."""
    pattern = re.compile(r'\s+'.join(map(re.escape, mutant['find'].split())))
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise MutantError(f'find text matches {len(matches)} times in {mutant["file"]}')
    match = matches[0]
    mutated = text[:match.start()] + mutant['replace'] + text[match.end():]
    if mutated == text:
        raise MutantError('replace leaves the file unchanged')
    return mutated


class Runner:
    def __init__(self, ref, tier, timeout):
        label = re.sub(r'[^A-Za-z0-9._-]', '_', ref) if ref else 'work-tree'
        base = ROOT / 'test-results/mutants' / label
        print(f'Copying {ref or "the work tree"} to {base.relative_to(ROOT)}', flush=True)
        self.tree = tree_support.materialize(base / 'tree', ref)
        self.build = self.tree / 'test-results/host'
        tree_support.configure(self.tree, self.build, tier)
        compilers = {'cc': tree_support.find_compiler('cc', 'CC'), 'c++': tree_support.find_compiler('c++', 'CXX')}
        for name in ('gcc', 'g++', 'clang', 'clang++'):
            if tree_support.host_compiler(name):
                compilers[name] = tree_support.host_compiler(name)
        wrappers = tree_support.write_compiler_wrappers(base / 'wrappers', compilers)
        self.failures = base / 'compile-failures.jsonl'
        self.environment = tree_support.wrapper_environment(wrappers, AMITY_WRAPPER_FAILURES=self.failures)
        self.timeout = timeout

    def test_run(self, regex, timeout=None):
        """Run the tests; return (outcome, detail) with outcome passed, failed or uncompiled."""
        self.failures.unlink(missing_ok=True)
        passed, failed, timed_out, output = tree_support.run_tests(self.build, regex, self.environment, timeout)
        if self.failures.exists() and self.failures.read_text().strip():
            failure = json.loads(self.failures.read_text().splitlines()[0])
            return 'uncompiled', '\n'.join(failure['errors'] or [' '.join(failure['command'])])
        if timed_out:
            return 'failed', 'timed out'
        if not passed:
            return 'failed', 'failed: ' + (', '.join(failed) or tail(output, 5))
        return 'passed', ''

    def manifest(self, path, manifest, baseline, only):
        if baseline and 'baseline' not in manifest:
            raise MutantError('the manifest names no baseline tests')
        spec = manifest['baseline'] if baseline else manifest
        regex = spec['ctest']
        tests = tree_support.list_tests(self.build, regex)
        if not tests:
            raise MutantError(f'no test matches {regex!r}')
        targets = spec.get('targets') or tree_support.test_targets(self.build, tests)
        print(f'{path.name}: {len(tests)} tests ({", ".join(tests[:4])}{", ..." if len(tests) > 4 else ""}), '
              f'targets {", ".join(targets) or "none"}', flush=True)
        tree_support.build_targets(self.build, targets)
        started = time.monotonic()
        outcome, detail = self.test_run(regex)
        if outcome != 'passed':
            raise MutantError(f'the unmutated tests do not pass ({detail})')
        timeout = self.timeout or 4 * (time.monotonic() - started) + 60
        mutants = [mutant for mutant in manifest['mutants'] if not only or mutant['id'] in only]
        results = {}
        for number, mutant in enumerate(mutants, 1):
            outcome, detail = self.mutant(mutant, regex, targets, timeout)
            results[mutant['id']] = {'outcome': outcome, 'detail': detail}
            note = ' (marked survives)' if 'survives' in mutant else ''
            print(f'  [{number}/{len(mutants)}] {mutant["id"]}: {outcome}{note}'
                  + (f' - {detail.splitlines()[0]}' if detail else ''), flush=True)
            if outcome == 'error' and '\n' in detail:
                print('    ' + detail.split('\n', 1)[1].replace('\n', '\n    '), flush=True)
        return {'ctest': regex, 'tests': tests, 'mutants': results}

    def mutant(self, mutant, regex, targets, timeout):
        path = self.tree / mutant['file']
        if not path.is_file():
            return 'error', f'{mutant["file"]} does not exist'
        original = path.read_bytes()
        try:
            mutated = mutate(original.decode(), mutant)
        except UnicodeDecodeError:
            return 'error', f'{mutant["file"]} is not UTF-8 text'
        except MutantError as error:
            return 'error', str(error)
        try:
            path.write_bytes(mutated.encode())
            try:
                tree_support.build_targets(self.build, targets)
            except Failure as failure:
                errors = [line for line in str(failure).splitlines() if re.search(r'(^|[\s:])error:', line)]
                return 'error', 'does not compile: ' + '\n'.join(errors[:3] or [tail(str(failure), 5)])
            outcome, detail = self.test_run(regex, timeout)
            if outcome == 'uncompiled':
                return 'error', 'does not compile: ' + detail
            return ('killed', detail) if outcome == 'failed' else ('survived', '')
        finally:
            path.write_bytes(original)


def compare(results, earlier_path):
    """Return the mutants killed in the earlier results that these results do not kill."""
    earlier = json.loads(Path(earlier_path).read_text())['manifests']
    shared = set(earlier) & set(results)
    if not shared:
        raise MutantError(f'{earlier_path} has no results for these manifests')
    lost = []
    for name in sorted(shared):
        for identifier, result in earlier[name]['mutants'].items():
            if result['outcome'] == 'killed':
                now = results[name]['mutants'].get(identifier, {}).get('outcome', 'not run')
                if now != 'killed':
                    lost.append(f'{name}: {identifier} was killed, now {now}')
    return lost


# Early-return stubs for the functions an old test slices.

def _imports(path):
    tree = ast.parse(path.read_text(), str(path))
    modules = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            modules.add(node.module)
            modules.update(f'{node.module}.{alias.name}' for alias in node.names)
    found = set()
    for module in modules:
        base = path.parent if path.parent.name != 'legacy_fixtures' else path.parent.parent
        candidate = base.joinpath(*module.split('.'))
        for file in (candidate.with_suffix('.py'), candidate / '__init__.py'):
            if file.is_file():
                found.add(file.resolve())
    return tree, found


def _constants(test):
    """Return the string constants of test and of the local modules it imports, except slice_support."""
    pending, done, constants = [Path(test).resolve()], set(), set()
    while pending:
        path = pending.pop()
        if path in done:
            continue
        done.add(path)
        tree, imports = _imports(path)
        constants.update(node.value for node in ast.walk(tree)
                         if isinstance(node, ast.Constant) and isinstance(node.value, str))
        pending.extend(module for module in imports if module.name != 'slice_support.py')
    return constants


def _referenced_files(constants):
    files, directories = set(), set()
    for constant in constants:
        words = constant.strip().split()
        relative = words[0].removeprefix('./').strip('/') if words else ''
        if not (relative + '/').startswith(FIRMWARE) and relative not in ('components', 'main'):
            continue
        path = ROOT / relative
        if path.is_file() and path.suffix in CODE_SUFFIXES:
            files.add(path)
        elif path.is_dir():
            directories.add(path)
    names = {constant for constant in constants if '/' not in constant and Path(constant).suffix in CODE_SUFFIXES}
    for directory in directories:
        for name in names:
            files.update(directory.rglob(name))
    return sorted(files)


def _blank(text):
    """Return text with comments, string and character literals and preprocessor lines blanked."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|^[ \t]*#(?:[^\n]*\\\n)*[^\n]*',
                         re.S | re.M)
    return pattern.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), text)


def _closing(blank, start, opening, closing):
    depth = 0
    for index in range(start, len(blank)):
        if blank[index] == opening:
            depth += 1
        elif blank[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    return -1


def _definition(blank, opening):
    """Return (declaration start, body brace, name, return type) for a definition whose parameters open at opening."""
    window = max(0, opening - 256)
    name = re.search(r'(~?)([A-Za-z_]\w*)\s*$', blank[window:opening])
    if not name or name.group(1) or name.group(2) in KEYWORDS:
        return None
    name_start = window + name.start(2)
    closing = _closing(blank, opening, '(', ')')
    if closing < 0:
        return None
    end = re.compile(r'[{};=]').search(blank, closing + 1)
    if not end or end.group() != '{':
        return None
    brace = end.start()
    trailer = blank[closing + 1:brace]
    if re.search(r'(?<!:):(?!:)', trailer) or re.search(r'[(),]', re.sub(r'(noexcept|__attribute__)\s*\(\(?[^)]*\)?\)', '', trailer)):
        return None
    boundary = max(blank.rfind(mark, 0, name_start) for mark in ';{}')
    label = [match.end() for match in re.finditer(r'(?<!:):(?!:)', blank[boundary + 1:name_start])]
    if label:
        boundary += label[-1]
    start = re.compile(r'\S').search(blank, boundary + 1).start()
    prefix = blank[start:name_start]
    prefix = re.sub(r'(?:[A-Za-z_]\w*\s*(?:<[^;{}]*>)?\s*::\s*)+$', '', prefix)
    prefix = re.sub(r'^\s*template\s*<[^;{}]*?>', '', prefix)
    prefix = re.sub(r'\[\[[^\]]*\]\]|__attribute__\s*\(\([^)]*\)\)', lambda match: ' noreturn ' if 'noreturn' in match.group() else ' ', prefix)
    words = [word for word in re.findall(r'[\w:<>,*&]+', prefix)
             if word not in SPECIFIERS and not re.fullmatch(r'[A-Z][A-Z0-9_]*_ATTR', word)]
    returns = re.sub(r'\s+', ' ', ' '.join(words)).strip()
    if returns == 'auto':
        trailing = re.search(r'->\s*([^{]+)$', trailer.strip())
        returns = trailing.group(1).strip() if trailing else 'auto'
    return start, brace, name.group(2), returns


def _statements(returns, c_source):
    words = returns.split()
    if not returns or 'auto' in words or 'noreturn' in words or 'constexpr' in words or 'consteval' in words \
            or returns.endswith('&'):
        return None
    if returns == 'void':
        return [('return', 'return;')]
    if returns == 'bool':
        return [('false', 'return false;'), ('true', 'return true;')]
    if returns == 'esp_err_t':
        return [('fail', 'return ESP_FAIL;')]
    return [('empty', f'return ({returns}){{0}};' if c_source else 'return {};')]


def stub_mutants(test):
    """Return early-return mutants for the firmware functions test names, and notes on what was skipped."""
    constants = _constants(test)
    anchors = [constant for constant in constants if '(' in constant and constant.strip()]
    names = [constant for constant in constants if re.fullmatch(r'[A-Za-z_]\w*', constant) and '_' in constant]
    mutants, notes, used = [], [], set()
    for path in _referenced_files(constants):
        text = path.read_text(errors='replace')
        blank = _blank(text)
        openings = set()
        for anchor in anchors:
            start = text.find(anchor)
            while start >= 0:
                opening = blank.find('(', start, start + len(anchor))
                if opening >= 0:
                    openings.add(opening)
                start = text.find(anchor, start + 1)
        for name in names:
            openings.update(match.end() - 1 for match in re.finditer(r'\b' + re.escape(name) + r'\s*\(', blank))
        definitions = {}
        for opening in sorted(openings):
            found = _definition(blank, opening)
            if found:
                definitions[found[1]] = found
        relative = path.relative_to(ROOT).as_posix()
        for start, brace, name, returns in sorted(definitions.values()):
            statements = _statements(returns, path.suffix == '.c')
            if not statements:
                notes.append(f'{relative}: {name}() returns {returns or "nothing"}; no stub')
                continue
            find = text[start:brace + 1]
            lines = text[:start].splitlines(keepends=True)
            pattern = lambda value: re.compile(r'\s+'.join(map(re.escape, value.split())))
            while len(pattern(find).findall(text)) > 1 and lines:
                find = lines.pop() + find
            if len(pattern(find).findall(text)) != 1:
                notes.append(f'{relative}: {name}() has no unique text to anchor a stub')
                continue
            find = find.strip()
            for suffix, statement in statements:
                identifier = f'stub-{name}-{suffix}'
                count = 2
                while identifier in used:
                    identifier, count = f'stub-{name}-{suffix}-{count}', count + 1
                used.add(identifier)
                mutants.append({'id': identifier, 'check': f'stub: early return from {name}()', 'file': relative,
                                'find': find, 'replace': f'{find} {statement}'})
    return mutants, notes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('manifests', nargs='*', type=Path, help='mutation manifests to run')
    parser.add_argument('--all', action='store_true', help='run every manifest in tests/mutants')
    parser.add_argument('--ref', help='run in a copy of this git ref instead of the work tree')
    parser.add_argument('--baseline', action='store_true',
                        help="run the manifest's baseline tests, the tests it replaced; survivors are allowed")
    parser.add_argument('--only', action='append', metavar='ID', help='run only this mutant; repeatable')
    parser.add_argument('--json', type=Path, help='write every outcome to this file')
    parser.add_argument('--require-kills', type=Path, metavar='JSON',
                        help='fail unless every mutant killed in this earlier --json file is killed again')
    parser.add_argument('--allow-survivors', action='store_true', help='exit 0 even when a mutant survives')
    parser.add_argument('--timeout', type=float, help='seconds per mutant (default: 4 times the unmutated run plus 60)')
    parser.add_argument('--tier', choices=('all', 'standalone', 'sdk'), help='host tests to define (default: all when '
                        'the SDK-built inputs exist, otherwise standalone)')
    parser.add_argument('--stubs', type=Path, metavar='TEST',
                        help='print a manifest skeleton with early-return mutants for the functions TEST slices')
    args = parser.parse_args(argv)

    if args.stubs:
        mutants, notes = stub_mutants(args.stubs)
        for note in notes:
            print(note, file=sys.stderr)
        name = args.stubs.name
        print(json.dumps({'ctest': '', 'baseline': {'ctest': '^' + re.escape(name) + '$'}, 'mutants': mutants},
                         indent=2))
        return 0
    manifests = list(args.manifests)
    if args.all:
        manifests += sorted(MANIFESTS.glob('*.json'))
    manifests = list({path.resolve(): path for path in manifests}.values())
    if not manifests:
        if args.all:
            print('No manifests in tests/mutants.')
            return 0
        parser.error('name a manifest or use --all')

    errors, survivors, results = [], [], {}
    try:
        loaded = [(path, load_manifest(path)) for path in manifests]
        if args.only:
            known = {mutant['id'] for _, manifest in loaded for mutant in manifest['mutants']}
            unknown = sorted(set(args.only) - known)
            if unknown:
                raise MutantError('unknown mutant ' + ', '.join(unknown))
        runner = Runner(args.ref, args.tier or tree_support.default_tier(), args.timeout)
    except (MutantError, Failure, json.JSONDecodeError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    for path, manifest in loaded:
        try:
            results[path.stem] = runner.manifest(path, manifest, args.baseline, set(args.only or ()))
        except (MutantError, Failure) as error:
            errors.append(f'{path.name}: {error}')
            print(f'error: {path.name}: {error}', file=sys.stderr)
            continue
        expected = {mutant['id'] for mutant in manifest['mutants'] if 'survives' in mutant}
        for identifier, result in results[path.stem]['mutants'].items():
            if result['outcome'] == 'error':
                errors.append(f'{path.name}: {identifier}: {result["detail"].splitlines()[0]}')
            elif result['outcome'] == 'survived' and identifier not in expected:
                survivors.append(f'{path.name}: {identifier}')
    lost = []
    if args.require_kills:
        try:
            lost = compare(results, args.require_kills)
        except (MutantError, OSError, KeyError, json.JSONDecodeError) as error:
            errors.append(f'--require-kills: {error}')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'ref': args.ref, 'baseline': args.baseline, 'manifests': results},
                                        indent=2) + '\n')
    outcomes = [result['outcome'] for run in results.values() for result in run['mutants'].values()]
    print(f'\n{outcomes.count("killed")} killed, {outcomes.count("survived")} survived, '
          f'{outcomes.count("error")} errors in {len(results)} of {len(loaded)} manifests')
    for title, items in (('Survived', survivors), ('No longer killed', lost), ('Errors', errors)):
        if items:
            print(f'{title}:\n  ' + '\n  '.join(items))
    if errors:
        return 2
    if lost or (survivors and not (args.allow_survivors or args.baseline)):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
