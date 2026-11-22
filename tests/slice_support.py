"""Cut code out of firmware source text for the Python tests that compile copies of it.

function() and block() return the braced code that begins at an exact text
anchor. named_function() finds a C function definition by name and skips braces
in strings and comments. An anchor must match once, except in the tail of a
file (source[source.index(marker):]), where the first match after the cut is
meant. A missing, ambiguous or unclosed anchor raises ValueError naming the file.

The text passed in is matched by content against the files under components/,
main/ and third_party/. A slice of a matched file is wrapped in #line
directives, so compiler diagnostics, sanitizer reports and gcov-style coverage
name the firmware file and line, and the test's own code after the slice is
attributed to <fixture>. After the first #line in a translation unit, GCC no
longer warns about misleading indentation. Slices of other text are returned
bare.
"""
from functools import lru_cache
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# The consortium sources outside the component's if(ESP_PLATFORM) block.
AMITY_CONSORTIUM_SOURCES = [ROOT / 'components/consortium' / name for name in (
    'bootstrap.cpp', 'enrollment.cpp', 'json.cpp', 'owned_intent.cpp', 'radio.cpp', 'session.cpp',
    'settings.cpp', 'update.cpp', 'wire.cpp')]

_TOKENS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)


def function(source: str, declaration: str) -> str:
    """Return the braced definition that begins at declaration, followed by a newline."""
    start = _anchor(source, declaration)
    return _located(source, start, _closing(source, start, declaration)) + '\n'


def block(source: str, marker: str) -> str:
    """Return the braced block, such as an if statement, that begins at marker."""
    start = _anchor(source, marker)
    return _located(source, start, _closing(source, start, marker))


def named_function(source: str, name: str) -> str:
    """Return the definition of the C function name."""
    pattern = re.compile(r'^[^\n;{}]*\b' + re.escape(name) + r'\([^;{}]*?\)\s*\{', re.M)
    matches = list(pattern.finditer(source))
    if not matches:
        raise ValueError(f'{_where(source)}: no definition of {name}()')
    if len(matches) > 1 and not _tail(source):
        raise ValueError(f'{_where(source)}: {len(matches)} definitions match {name}()')
    start = matches[0]
    depth = 0
    for token in _TOKENS.finditer(source, start.end() - 1):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return _located(source, start.start(), token.end())
    raise ValueError(f'{_where(source)}: {name}() is not closed')


def _anchor(source, anchor):
    """Return where anchor starts in source."""
    start = source.find(anchor)
    if start < 0:
        raise ValueError(f'{_where(source)}: {anchor!r} not found')
    count = source.count(anchor)
    if count > 1 and not _tail(source):
        raise ValueError(f'{_where(source)}: {anchor!r} matches {count} times')
    return start


def _closing(source, start, anchor):
    """Return the index after the brace that closes the first one at or after start."""
    opening = source.find('{', start)
    if opening >= 0:
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == '{':
                depth += 1
            elif source[index] == '}':
                depth -= 1
                if depth == 0:
                    return index + 1
    raise ValueError(f'{_where(source)}: no closed braces after {anchor!r}')


def _located(source, start, end):
    """Return source[start:end], wrapped in #line directives when source comes from a file."""
    origin = _origin(source)
    if not origin:
        return source[start:end]
    path, offset, text = origin
    line = text.count('\n', 0, offset + start) + 1
    name = str(path).replace('\\', '\\\\').replace('"', '\\"')
    # The comment keeps a semicolon that a caller appends after rstrip() off the directive line.
    return f'\n#line {line} "{name}"\n{source[start:end]}\n#line 1 "<fixture>"\n/* end of slice */'


def _where(source):
    origin = _origin(source)
    if not origin:
        return 'source text'
    path, offset, text = origin
    name = str(path.relative_to(ROOT))
    if not offset:
        return name
    line = text.count('\n', 0, offset) + 1
    return f'{name} from line {line}'


def _tail(source):
    origin = _origin(source)
    return bool(origin and origin[1])


@lru_cache(maxsize=None)
def _origin(source):
    """Return the file whose text is source, or ends with it, as (path, offset, file text)."""
    files = _files()
    path = files.get(source)
    if path:
        return path, 0, source
    tails = [(name, text) for text, name in files.items()
             if name and len(text) > len(source) and text.endswith(source)]
    if len(tails) == 1:
        path, text = tails[0]
        return path, len(text) - len(source), text
    return None


@lru_cache(maxsize=None)
def _files():
    """Map the text of each firmware source file to its path, or to None when files share it."""
    files = {}
    for root in ('components', 'main', 'third_party'):
        for path in sorted((ROOT / root).rglob('*')):
            if path.suffix not in {'.c', '.cc', '.cpp', '.h', '.hpp', '.inc'} or not path.is_file():
                continue
            try:
                text = path.read_text()
            except UnicodeDecodeError:
                continue
            files[text] = None if text in files else path
    return files
