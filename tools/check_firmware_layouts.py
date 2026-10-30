#!/usr/bin/env python3
"""Reject an ELF containing inconsistent public C++ record layouts across objects.

This catches a public header changing while a parallel incremental build is
running, before an otherwise linkable incompatible image reaches the device.
"""
import argparse
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile

RECORDS={
    'amity::system::ExecutionSnapshot','amity::system::ActivityOutcome','amity::system::InputIntent','amity::system::SelectRequest',
    'amity::system::CommandOutcome','amity::system::LocalConfiguration',
    'amity::system::PortPolicySnapshot','amity::system::ComponentBinding',
    'amity::system::CecObservation','amity::system::Topology','amity::system::Plan',
    'amity::system::AuthorizedCecOperation','amity::system::WireWork',
    'amity::system::OperationArguments','amity::system::LocalCecAgent',
    'amity::system::Coordinator',
    'amity::system::LocalCecAgent::Transmission','amity::system::LocalCecAgent::HeldKey',
    'amity::system::LocalCecAgent::AudioQuery','amity::system::LocalCecAgent::ReleaseStream',
    'amity::system::Coordinator::Lane','amity::system::Coordinator::WireAdapter',
    'amity::consortium::RuntimeStatus','amity::consortium::RuntimePeerStatus',
    'amity::consortium::RuntimeControlHooks','amity::consortium::RuntimeNetworkHooks','amity::consortium::Envelope',
    'amity::system::OwnedIntent',
    'amity::consortium::Operation','config_store::Configuration',
    'config_store::ConfiguredComponent',
    'amity::Snapshot','cec::ExecutionFence','cec::Completion','cec::Submission','cec::Engine',
    'config_store::RequestedSession',
    'amity::system::RequestedActivity','amity::ConfigurationActivationStatus',
    'amity::consortium::RuntimeLifecycleHooks',
    'amity::consortium::RuntimeSettingsHooks',
    'amity::consortium::DeviceSettingsWrite','amity::consortium::AdapterSettingChange',
    'amity::AdapterRuntimeState',
    'device_settings::AdapterDefaults','device_settings::AdapterDefaultChange',
    'system_services::updates::ReleaseManifest',
    'config_store::ReplacementApproval','config_store::ReplacementPort','config_store::ReplacementEquipment',
    'amity::HubService','amity::SystemControl','amity::LiveSystem',
    'amity::LiveSystem::Port','amity::LiveSystem::Port::Submitted',
    'cec::BoardProfile','cec::PresencePin','cec::PortPin','cec::Bus',
    'amity::consortium::HelloFrame',
    'amity::consortium::platform::TlsLink',
}
RECORD_NAMES={n.rsplit('::',1)[-1] for n in RECORDS}
ROOT=Path(__file__).resolve().parents[1]


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


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--elf',required=True,type=Path);parser.add_argument('--output',type=Path);args=parser.parse_args()
    records={};errors=[]
    with args.elf.open('rb') as stream:
        elf=ELFFile(stream)
        if not elf.has_dwarf_info():raise SystemExit('ELF has no DWARF layout evidence')
        for cu in elf.get_dwarf_info().iter_CUs():
            source=name(cu.get_top_DIE())
            if not source.startswith(str(ROOT/'components')+'/') and source!=str(ROOT/'main/main.cpp'):continue
            for die in cu.iter_DIEs():
                if die.tag not in ('DW_TAG_structure_type','DW_TAG_class_type') or 'DW_AT_byte_size' not in die.attributes:continue
                if name(die) not in RECORD_NAMES:continue
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
    result=dict(elf=str(args.elf),records=records,errors=errors)
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2)+'\n')
    if errors:
        for error in errors:print('Inconsistent public layout:',error['record'],error['first']['source'],error['first']['layout']['size'],'!=',error['conflicting']['source'],error['conflicting']['layout']['size'])
        raise SystemExit('Public headers changed during the build. Rebuild before flashing.')
    print('Consistent firmware layouts:',len(records),'public records')


if __name__=='__main__':main()
