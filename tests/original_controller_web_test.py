#!/usr/bin/env python3
"""Keep whole-heap inspection out of the administration server and startup."""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]

def main():
    # Heap inspection walks every allocation with interrupts masked. Even
    # read-only diagnostics must not interrupt a CEC frame after startup.
    for relative in ('components/web_admin/web_admin.cpp',
                     'components/web_admin/activity_editor.cpp', 'main/main.cpp'):
        diagnostic_source=(ROOT/relative).read_text()
        assert not re.search(r'\bheap_caps_get_(?:info|largest_free_block)\s*\(', diagnostic_source), relative

if __name__=='__main__':main()
