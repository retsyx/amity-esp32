#!/usr/bin/env python3
"""Reject an ELF in which an object left the memory it must live in.

DRAM_ATTR and EXT_RAM_BSS_ATTR only request a memory; the configuration and
the linker script decide where the object lands, and no host test can see it.
Each listed object must exist in the ELF's symbol table and lie in one of its
output sections, so the list cannot silently go stale.
"""
import argparse
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

INTERNAL_DRAM = ('internal DRAM', {'.dram0.data', '.dram0.bss'})
PSRAM = ('PSRAM', {'.ext_ram.bss'})

# Mangled symbol: (object, (memory, output sections)).
PLACEMENTS = {
    # GAP/GATT callbacks copy admission records into the work queue; its
    # control block stays in internal memory, its 16 records in PSRAM.
    '_ZN5amity10consortium8platform12_GLOBAL__N_1L18work_queue_controlE':
        ('amity::consortium::platform::work_queue_control', INTERNAL_DRAM),
    '_ZN5amity10consortium8platform12_GLOBAL__N_1L18work_queue_storageE':
        ('amity::consortium::platform::work_queue_storage', PSRAM),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', required=True)
    parser.add_argument('--output')
    args = parser.parse_args()
    # The output marks a passed check; a failed check must run again.
    if args.output and Path(args.output).exists():
        Path(args.output).unlink()
    found = {}
    with open(args.elf, 'rb') as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name('.symtab')
        if not isinstance(table, SymbolTableSection):
            raise SystemExit('ELF has no symbol table')
        for symbol in table.iter_symbols():
            if symbol.name in PLACEMENTS and isinstance(symbol['st_shndx'], int):
                found[symbol.name] = elf.get_section(symbol['st_shndx']).name
    errors = []
    for mangled, (name, (memory, sections)) in sorted(PLACEMENTS.items()):
        if mangled not in found:
            errors.append(f'{name} is not in the ELF; update PLACEMENTS in tools/check_firmware_placement.py')
        elif found[mangled] not in sections:
            errors.append(f'{name} is in {found[mangled]}, not in {memory}')
    if errors:
        raise SystemExit('\n'.join(errors))
    if args.output:
        Path(args.output).write_text(''.join(f'{PLACEMENTS[mangled][0]} {section}\n'
                                             for mangled, section in sorted(found.items())))
    print('Firmware placements hold:', len(found), 'objects')


if __name__ == '__main__':
    main()
