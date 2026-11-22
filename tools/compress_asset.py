#!/usr/bin/env python3
"""Gzip one administration asset reproducibly: maximum compression, no name or time."""
import gzip
from pathlib import Path
import sys

source, output = (Path(argument) for argument in sys.argv[1:3])
output.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
