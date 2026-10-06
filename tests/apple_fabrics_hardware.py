#!/usr/bin/env python3
"""Exercise Apple fabric cleanup using isolated test controllers on the Pi.

Run with the same private mount namespace and CHIP Python environment documented
in matter_pi.md. Commissioning creates test fabrics using Apple's vendor IDs;
it does not use Apple services or change the DUT's WiFi network. The caller must
open the DUT's commissioning window before each commission action. Native SDK
output can contain credentials and must be saved to a private log.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--storage', type=Path, required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--trust-store', type=Path, required=True)
    parser.add_argument('--address', required=True)
    parser.add_argument('--role', choices=('observer', 'keychain', 'home1', 'home2'), required=True)
    parser.add_argument('--action', choices=('commission', 'read', 'remove'), required=True)
    parser.add_argument('--index', type=int)
    parser.add_argument('--expected-version', default='0.1.38')
    args = parser.parse_args()
    if args.action == 'remove' and (args.index is None or not 1 <= args.index <= 254):
        parser.error('--action remove requires --index in 1..254')
    if args.credentials.stat().st_mode & 0o077:
        parser.error('Credentials must have mode 600')
    os.umask(0o077)
    args.storage.mkdir(parents=True, exist_ok=True)
    platform = args.storage / 'platform'
    platform.mkdir(exist_ok=True)
    subprocess.run(['mount', '--bind', str(platform.resolve()), '/tmp'], check=True)

    import chip.native
    from chip.ChipStack import ChipStack
    from chip.CertificateAuthority import CertificateAuthorityManager
    import chip.clusters as Clusters

    roles = {'observer': (0xFFF1, 101), 'keychain': (0x1384, 102),
             'home1': (0x1349, 103), 'home2': (0x1349, 104)}
    vendor, fabric_id = roles[args.role]
    node = 0x1234
    result = {'passed': False, 'role': args.role, 'action': args.action}
    stack = manager = controller = None
    try:
        chip.native.Init(bluetoothAdapter=0)
        stack = ChipStack(str(args.storage / 'controller.json'))
        manager = CertificateAuthorityManager(stack)
        manager.LoadAuthoritiesFromStorage()
        ca = manager.activeCaList[0] if manager.activeCaList else manager.NewCertificateAuthority(caIndex=1)
        admin = next((admin for admin in ca.adminList if admin.fabricId == fabric_id), None)
        if admin is None:
            admin = ca.NewFabricAdmin(vendorId=vendor, fabricId=fabric_id)
        controller = admin.NewController(nodeId=112233, paaTrustStorePath=str(args.trust_store))

        async def run():
            if args.action == 'commission':
                credentials = json.loads(args.credentials.read_text())
                # The pinned test controller still provides this direct-IP API.
                # It avoids harness mDNS routing and does not reprovision WiFi.
                await asyncio.wait_for(controller.CommissionIP(args.address, credentials['setup_pin'], node), 90)
            if args.action == 'remove':
                response = await controller.SendCommand(node, 0,
                    Clusters.OperationalCredentials.Commands.RemoveFabric(fabricIndex=args.index))
                assert response.statusCode == 0
                result['removed_index'] = args.index
            values = await controller.ReadAttribute(node, [
                (0, Clusters.OperationalCredentials.Attributes.CurrentFabricIndex),
                (0, Clusters.BasicInformation.Attributes.SoftwareVersionString)])
            result['index'] = values[0][Clusters.OperationalCredentials][Clusters.OperationalCredentials.Attributes.CurrentFabricIndex]
            result['version'] = values[0][Clusters.BasicInformation][Clusters.BasicInformation.Attributes.SoftwareVersionString]
            assert result['version'] == args.expected_version
            result['passed'] = True

        asyncio.run(run())
    except Exception as err:
        result['error'] = type(err).__name__ + ': ' + str(err)
    finally:
        (args.storage / f'{args.role}-{args.action}.json').write_text(json.dumps(result, indent=2))
        if controller is not None:
            controller.Shutdown()
        if manager is not None:
            manager.Shutdown()
        if stack is not None:
            stack.Shutdown()
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
