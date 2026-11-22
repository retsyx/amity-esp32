#!/usr/bin/env python3
"""Reject an ELF containing inconsistent public C++ record layouts across objects.

This catches a public header changing while a parallel incremental build is
running, before an otherwise linkable incompatible image reaches the device.
Every listed record must still exist, so the list cannot silently go stale.
"""
import argparse
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile

RECORDS={
    'amity::system::ExecutionSnapshot','amity::system::ActivityOutcome','amity::system::CecObservation','amity::system::OwnedIntent',
    'amity::consortium::RuntimeStatus','amity::consortium::RuntimePeerStatus',
    'amity::consortium::RuntimeControlHooks','amity::consortium::RuntimeNetworkHooks','amity::consortium::Envelope',
    'amity::consortium::RuntimeLifecycleHooks','amity::consortium::RuntimeSettingsHooks',
    'amity::consortium::DeviceSettingsWrite','amity::consortium::AdapterSettingChange',
    'amity::consortium::HelloFrame','amity::consortium::platform::TlsLink',
    'config_store::Configuration','config_store::ConfiguredComponent',
    'config_store::ReplacementApproval','config_store::ReplacementPort','config_store::ReplacementEquipment',
    'amity::Snapshot','amity::ConfigurationActivationStatus','amity::AdapterRuntimeState',
    'amity::HubService','amity::SystemControl',
    'cec::ExecutionFence','cec::Engine','cec::BoardProfile','cec::PortPin','cec::Bus',
    'device_settings::AdapterDefaults','device_settings::AdapterDefaultChange',
    'system_services::updates::ReleaseManifest',
}
RECORD_NAMES={n.rsplit('::',1)[-1] for n in RECORDS}
ROOT=Path(__file__).resolve().parents[1]
# Function bodies hold only local types; skipping them avoids most DWARF parsing.
SKIPPED={'DW_TAG_subprogram','DW_TAG_inlined_subroutine','DW_TAG_lexical_block','DW_TAG_variable',
         'DW_TAG_formal_parameter','DW_TAG_enumeration_type'}


def name(die):
    value=die.attributes.get('DW_AT_name')
    return value.value.decode(errors='replace') if value else ''


def qualified(die):
    values=[]
    while die:
        if die.tag in ('DW_TAG_namespace','DW_TAG_structure_type','DW_TAG_class_type') and name(die):values.append(name(die))
        die=die.get_parent()
    return '::'.join(reversed(values))


def type_name(die,depth=0):
    if not die:return 'void'
    if depth>12:return die.tag
    if name(die):
        if die.tag in ('DW_TAG_structure_type','DW_TAG_class_type','DW_TAG_enumeration_type'):return qualified(die) or name(die)
        return name(die)
    target=die.get_DIE_from_attribute('DW_AT_type') if 'DW_AT_type' in die.attributes else None
    if die.tag=='DW_TAG_subroutine_type':
        args=[type_name(c.get_DIE_from_attribute('DW_AT_type'),depth+1) for c in die.iter_children() if c.tag=='DW_TAG_formal_parameter' and 'DW_AT_type' in c.attributes]
        return type_name(target,depth+1)+'('+','.join(args)+')'
    return die.tag+':'+type_name(target,depth+1)


def records_in(die):
    for child in die.iter_children():
        if child.tag in SKIPPED:continue
        if child.tag in ('DW_TAG_structure_type','DW_TAG_class_type'):yield child
        if child.has_children:yield from records_in(child)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--elf',required=True,type=Path);parser.add_argument('--output',type=Path);args=parser.parse_args()
    records={};errors=[]
    with args.elf.open('rb') as stream:
        elf=ELFFile(stream)
        if not elf.has_dwarf_info():raise SystemExit('ELF has no DWARF layout evidence')
        for cu in elf.get_dwarf_info().iter_CUs():
            top=cu.get_top_DIE();source=name(top)
            if not source.startswith(str(ROOT/'components')+'/') and source!=str(ROOT/'main/main.cpp'):continue
            for die in records_in(top):
                if 'DW_AT_byte_size' not in die.attributes or name(die) not in RECORD_NAMES:continue
                key=qualified(die)
                if key not in RECORDS:continue
                members=[]
                for member in die.iter_children():
                    if member.tag!='DW_TAG_member':continue
                    location=member.attributes.get('DW_AT_data_member_location')
                    target=member.get_DIE_from_attribute('DW_AT_type') if 'DW_AT_type' in member.attributes else None
                    members.append((name(member),location.value if location else None,type_name(target)))
                layout=dict(size=die.attributes['DW_AT_byte_size'].value,members=members)
                prior=records.setdefault(key,[])
                if prior and prior[0]['layout']!=layout:errors.append(dict(record=key,first=prior[0],conflicting=dict(source=source,layout=layout)))
                prior.append(dict(source=source,layout=layout))
    # The output marks a passed check; a failed check must run again.
    if args.output and args.output.exists():args.output.unlink()
    if errors:
        for error in errors:print('Inconsistent public layout:',error['record'],error['first']['source'],error['first']['layout']['size'],'!=',error['conflicting']['source'],error['conflicting']['layout']['size'])
        raise SystemExit('Public headers changed during the build. Rebuild before flashing.')
    missing=sorted(RECORDS-set(records))
    if missing:raise SystemExit('Update RECORDS in tools/check_firmware_layouts.py; these records no longer exist: '+', '.join(missing))
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(dict(elf=str(args.elf),records=records),indent=2)+'\n')
    print('Consistent firmware layouts:',len(records),'public records')


if __name__=='__main__':main()
