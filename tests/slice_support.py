"""Slicing helpers for the Python tests that compile copies of firmware code."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# The consortium sources outside the component's if(ESP_PLATFORM) block.
AMITY_CONSORTIUM_SOURCES = [ROOT / 'components/consortium' / name for name in (
    'bootstrap.cpp', 'enrollment.cpp', 'json.cpp', 'owned_intent.cpp', 'radio.cpp', 'session.cpp',
    'settings.cpp', 'update.cpp', 'wire.cpp')]


def function(source: str, declaration: str) -> str:
    start = source.index(declaration)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(marker)


def named_function(source: str, name: str) -> str:
    start = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*?\)\s*\{", source, re.M)
    if not start:
        raise AssertionError(f"Production function not found: {name}")
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, start.end() - 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start.start():token.end()]
    raise AssertionError(f"Unclosed production function: {name}")
