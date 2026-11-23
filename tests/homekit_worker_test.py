#!/usr/bin/env python3
"""Check that the HomeKit worker task holds nothing alive when it deletes itself.

worker() runs worker_loop() in its own frame and then deletes its own task
with vTaskDeleteWithCaps(), which never returns: anything worker() itself held
would never be destroyed. worker_loop() never returns, so no host test reaches
the delete, and homekit_test.cpp cannot observe it; this checks the shape of
worker() instead.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'components/amity_homekit/homekit.cpp').read_text()
    workers = re.findall(r'^void worker\(void \*\)\{\n(.*?)^\}', source, re.S | re.M)
    assert len(workers) == 1, 'homekit.cpp must define worker(void *) once'
    body = re.sub(r'\s+', '', workers[0])
    assert body == 'worker_loop();vTaskDeleteWithCaps(nullptr);', \
        'worker() must run worker_loop() and then delete its own task, holding nothing itself'
    print('PASS HomeKit worker task entry: the work loop runs in its own frame before the task deletes itself')


if __name__ == '__main__':
    main()
