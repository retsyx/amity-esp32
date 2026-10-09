#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Isolated Linux Matter controller fallback using the official CHIP Python API.

Dependencies can be extracted into an isolated PYTHONPATH; no system packages
are required. Run under `sudo unshare --mount --propagation private` with
--isolate-platform-tmp to keep CHIP's fixed /tmp platform files in --storage.
Credentials contain ssid, password, setup_pin and discriminator; keep mode 600.
All output may contain commissioning material and must go to a private log.

The Home Assistant CHIP 2025.7.0 wheel has three /data/chip_* storage constants.
For an isolated harness without /data, relocate those constants in the local
test-only native library copy to the equal-length /tmp//chip_* prefix before
running. The private mount namespace then contains all platform writes in the
requested storage directory. Do not alter the system's /data or /tmp.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--storage", type=Path, required=True)
    parser.add_argument("--credentials", type=Path)
    parser.add_argument("--pase-only", action="store_true",
                        help="Validate clusters over BLE PASE without provisioning WiFi or a fabric")
    parser.add_argument("--pase-address", help="Use PASE over IP at this address instead of BLE; requires --pase-only")
    parser.add_argument("--replace-network", action="store_true",
                        help="Remove a different staged WiFi network under the commissioning failsafe")
    parser.add_argument("--commission-gate", type=Path,
                        help="After PASE, wait at most 180 seconds for this file before replacing the network")
    parser.add_argument("--switch-network", type=Path,
                        help="Move an existing fabric to the private ssid/password file under a failsafe")
    parser.add_argument("--complete-staged", action="store_true",
                        help="Complete commissioning over CASE using a fabric already staged under the failsafe")
    parser.add_argument("--handoff-status", type=Path,
                        help="Test harness marker: extend the failsafe after successful WiFi connection before CASE handoff")
    parser.add_argument("--trust-store", type=Path, required=True)
    parser.add_argument("--node", type=lambda s: int(s, 0), default=0x1234)
    parser.add_argument("--expect-battery", type=int, help="Remote percentage; -1 expects null")
    parser.add_argument("--expect-charging", choices=("yes", "no", "unknown"))
    parser.add_argument("--expected-version", help="Assert the Matter Basic Information software version string")
    parser.add_argument("--expect-input-count", type=int, help="Assert the published activity count")
    parser.add_argument("--isolate-platform-tmp", action="store_true")
    parser.add_argument("--expect-unknown-audio", action="store_true")
    parser.add_argument("--expect-volume", type=int, help="Assert an observed Matter level (0..254)")
    parser.add_argument("--expect-muted", choices=("yes", "no"), help="Assert observed CEC mute feedback")
    parser.add_argument("--speaker-mute", choices=("muted", "unmuted"),
                        help="Send one speaker On/Off command; verify its CEC effect separately")
    parser.add_argument("--exercise-controls", action="store_true",
                        help="Send playback, keypad and relative volume commands to the active activity")
    parser.add_argument("--exercise-power-inputs", action="store_true",
                        help="Exercise player power and each configured input, then restore the initial state")
    parser.add_argument("--volume-step", type=int,
                        help="Send one signed relative volume step count after validation")
    parser.add_argument("--command-marker", type=Path,
                        help="Write the accepted volume command timestamp here for synchronized CEC tests")
    args = parser.parse_args()
    if args.pase_only and not args.credentials:
        parser.error("--pase-only requires --credentials with setup_pin and discriminator")
    if args.pase_address and not args.pase_only:
        parser.error("--pase-address requires --pase-only")
    if args.replace_network and (args.pase_only or not args.credentials):
        parser.error("--replace-network requires WiFi commissioning credentials")
    if args.switch_network and args.credentials:
        parser.error("--switch-network reuses an existing fabric; omit --credentials")
    if args.complete_staged and (args.credentials or args.switch_network):
        parser.error("--complete-staged reuses an already staged fabric; omit credential options")
    if args.handoff_status:
        if not args.credentials or args.pase_only or args.replace_network:
            parser.error("--handoff-status requires ordinary WiFi commissioning")
        if not args.handoff_status.resolve().is_relative_to(args.storage.resolve()):
            parser.error("--handoff-status must be within --storage")
        if args.handoff_status.exists() or Path(str(args.handoff_status) + ".extended").exists():
            parser.error("Handoff markers must be fresh")
    if args.volume_step is not None and not -254 <= args.volume_step <= 254:
        parser.error("--volume-step must be between -254 and 254")
    if args.expect_volume is not None and not 0 <= args.expect_volume <= 254:
        parser.error("--expect-volume must be between 0 and 254")
    if args.expect_input_count is not None and not 0 <= args.expect_input_count <= 16:
        parser.error("--expect-input-count must be between 0 and 16")
    if args.expect_unknown_audio and (args.expect_volume is not None or args.expect_muted is not None):
        parser.error("Observed audio expectations cannot be combined with --expect-unknown-audio")
    if args.command_marker and (args.volume_step is None or args.command_marker.exists()):
        parser.error("--command-marker requires --volume-step and a fresh marker path")
    if args.commission_gate:
        if not args.replace_network or not args.commission_gate.resolve().is_relative_to(args.storage.resolve()):
            parser.error("--commission-gate requires --replace-network and a path within --storage")
        if args.commission_gate.exists():
            parser.error("Commissioning gate already exists; use a fresh gate path")
    os.umask(0o077)
    args.storage.mkdir(parents=True, exist_ok=True)
    if args.isolate_platform_tmp:
        platform = args.storage / "platform"
        platform.mkdir(exist_ok=True)
        # The caller creates a private mount namespace, so this leaves the
        # harness's real /tmp and all other processes' storage untouched.
        subprocess.run(["mount", "--bind", str(platform.resolve()), "/tmp"], check=True)

    import chip.native
    from chip.ChipStack import ChipStack
    from chip.CertificateAuthority import CertificateAuthorityManager
    import chip.clusters as Clusters
    from chip.exceptions import ChipStackError
    from chip.interaction_model import InteractionModelError, Status

    result = {"node": args.node, "passed": False}
    stack = manager = controller = None
    try:
        chip.native.Init(bluetoothAdapter=0)
        stack = ChipStack(str(args.storage / "controller.json"))
        manager = CertificateAuthorityManager(stack)
        manager.LoadAuthoritiesFromStorage()
        ca = manager.activeCaList[0] if manager.activeCaList else manager.NewCertificateAuthority(caIndex=1)
        admin = ca.adminList[0] if ca.adminList else ca.NewFabricAdmin(vendorId=0xFFF1, fabricId=1)
        controller = admin.NewController(nodeId=112233, paaTrustStorePath=str(args.trust_store))

        async def run():
            if args.complete_staged:
                response = await asyncio.wait_for(controller.SendCommand(args.node, 0,
                    Clusters.GeneralCommissioning.Commands.CommissioningComplete(),
                    interactionTimeoutMs=20000), 25)
                assert response.errorCode == 0
                result["staged_commissioning_completed"] = True
            if args.switch_network:
                if args.switch_network.stat().st_mode & 0o077:
                    raise RuntimeError("Network credentials must have mode 600")
                network_credentials = json.loads(args.switch_network.read_text())
                network_id = network_credentials["ssid"].encode("utf-8")
                response = await controller.SendCommand(args.node, 0,
                    Clusters.GeneralCommissioning.Commands.ArmFailSafe(expiryLengthSeconds=180, breadcrumb=0))
                assert response.errorCode == 0
                networks = await controller.ReadAttribute(args.node,
                    [(0, Clusters.NetworkCommissioning.Attributes.Networks)])
                for network in networks[0][Clusters.NetworkCommissioning][Clusters.NetworkCommissioning.Attributes.Networks]:
                    if network.networkID != network_id:
                        response = await controller.SendCommand(args.node, 0,
                            Clusters.NetworkCommissioning.Commands.RemoveNetwork(networkID=network.networkID, breadcrumb=0))
                        assert response.networkingStatus == 0
                response = await controller.SendCommand(args.node, 0,
                    Clusters.NetworkCommissioning.Commands.AddOrUpdateWiFiNetwork(
                        ssid=network_id, credentials=network_credentials["password"].encode("utf-8"), breadcrumb=0))
                assert response.networkingStatus == 0
                try:
                    response = await controller.SendCommand(args.node, 0,
                        Clusters.NetworkCommissioning.Commands.ConnectNetwork(networkID=network_id, breadcrumb=0),
                        interactionTimeoutMs=15000)
                    assert response.networkingStatus == 0
                except ChipStackError:
                    # Changing interfaces can lose the old session's response.
                    # Only a successful CASE CommissioningComplete below commits
                    # the switch; otherwise the device's failsafe restores it.
                    result["network_response_lost"] = True
                controller.ExpireSessions(args.node)
                response = await asyncio.wait_for(controller.SendCommand(args.node, 0,
                    Clusters.GeneralCommissioning.Commands.CommissioningComplete(),
                    interactionTimeoutMs=90000), 95)
                assert response.errorCode == 0
                result["network_switched"] = True
            if args.credentials:
                if args.credentials.stat().st_mode & 0o077:
                    raise RuntimeError("Credentials must have mode 600")
                credentials = json.loads(args.credentials.read_text())
                if args.pase_only:
                    if args.pase_address:
                        await asyncio.wait_for(controller.EstablishPASESessionIP(
                            args.pase_address, credentials["setup_pin"], args.node), 60)
                        result["transport"] = "IP PASE"
                    else:
                        await asyncio.wait_for(controller.EstablishPASESessionBLE(
                            credentials["setup_pin"], credentials["discriminator"], args.node), 60)
                        result["transport"] = "BLE PASE"
                elif args.replace_network:
                    await asyncio.wait_for(controller.EstablishPASESessionBLE(
                        credentials["setup_pin"], credentials["discriminator"], args.node), 60)
                    if args.commission_gate:
                        # The DUT automatically arms a 60-second failsafe when
                        # PASE starts. Extend it before the harness AP setup,
                        # without changing network configuration before release.
                        response = await controller.SendCommand(args.node, 0,
                            Clusters.GeneralCommissioning.Commands.ArmFailSafe(expiryLengthSeconds=180, breadcrumb=0))
                        assert response.errorCode == 0
                        ready = Path(str(args.commission_gate) + ".ready.json")
                        ready.write_text(json.dumps({"node": args.node, "pase_ready": True, "waiting": True}))
                        deadline = asyncio.get_running_loop().time() + 180
                        while not args.commission_gate.exists():
                            if asyncio.get_running_loop().time() >= deadline:
                                raise TimeoutError("Commissioning gate was not released within 180 seconds")
                            # Keep the established BLE session active while the
                            # harness changes its WiFi interface role. The
                            # failsafe is refreshed after the AP is ready.
                            await asyncio.wait_for(controller.ReadAttribute(args.node,
                                [(1, Clusters.Descriptor.Attributes.DeviceTypeList)]), 10)
                            for _ in range(25):
                                if args.commission_gate.exists():
                                    break
                                await asyncio.sleep(0.2)
                        ready.write_text(json.dumps({"node": args.node, "pase_ready": True, "waiting": False}))
                        result["pase_gate_used"] = True
                    response = await controller.SendCommand(args.node, 0,
                        Clusters.GeneralCommissioning.Commands.ArmFailSafe(expiryLengthSeconds=180, breadcrumb=0))
                    assert response.errorCode == 0
                    networks = await controller.ReadAttribute(args.node,
                        [(0, Clusters.NetworkCommissioning.Attributes.Networks)])
                    for network in networks[0][Clusters.NetworkCommissioning][Clusters.NetworkCommissioning.Attributes.Networks]:
                        if network.networkID != credentials["ssid"].encode("utf-8"):
                            response = await controller.SendCommand(args.node, 0,
                                Clusters.NetworkCommissioning.Commands.RemoveNetwork(networkID=network.networkID, breadcrumb=0))
                            assert response.networkingStatus == 0
                    controller.SetWiFiCredentials(credentials["ssid"], credentials["password"])
                    await asyncio.wait_for(controller.Commission(args.node), 180)
                    result["commissioned"] = True
                else:
                    async def commission_wifi():
                        commissioning = asyncio.create_task(controller.CommissionWiFi(
                            credentials["discriminator"], credentials["setup_pin"], args.node,
                            credentials["ssid"], credentials["password"]))
                        if args.handoff_status:
                            while not commissioning.done() and not args.handoff_status.exists():
                                await asyncio.sleep(0.1)
                            if args.handoff_status.exists() and not commissioning.done():
                                response = await controller.SendCommand(args.node, 0,
                                    Clusters.GeneralCommissioning.Commands.ArmFailSafe(
                                        expiryLengthSeconds=300, breadcrumb=0), interactionTimeoutMs=8000)
                                assert response.errorCode == 0
                                Path(str(args.handoff_status) + ".extended").write_text(json.dumps({
                                    "failsafe_seconds": 300, "monotonic_ns": time.monotonic_ns()}))
                        return await commissioning

                    await asyncio.wait_for(commission_wifi(), 180)
                    result["commissioned"] = True
            read = await asyncio.wait_for(controller.ReadAttribute(args.node, [
                (0, Clusters.Descriptor.Attributes.PartsList),
                (1, Clusters.Descriptor.Attributes.DeviceTypeList),
                (2, Clusters.Descriptor.Attributes.DeviceTypeList),
                (1, Clusters.Descriptor.Attributes.ServerList),
                (2, Clusters.Descriptor.Attributes.ServerList),
                (1, Clusters.MediaInput.Attributes.InputList),
                (1, Clusters.MediaInput.Attributes.CurrentInput),
                (1, Clusters.MediaPlayback.Attributes.CurrentState),
                (1, Clusters.KeypadInput.Attributes.FeatureMap),
                (1, Clusters.OnOff.Attributes.OnOff),
                (2, Clusters.OnOff.Attributes.OnOff),
                (2, Clusters.LevelControl.Attributes.CurrentLevel),
                (0, Clusters.OperationalCredentials.Attributes.CommissionedFabrics),
                (0, Clusters.BasicInformation.Attributes.SoftwareVersion),
                (0, Clusters.BasicInformation.Attributes.SoftwareVersionString),
            ]), 30)
            if args.expect_battery is not None or args.expect_charging is not None:
                battery_read = await controller.ReadAttribute(args.node, [(3, Clusters.PowerSource), (3, Clusters.Descriptor), (0, Clusters.Descriptor.Attributes.PartsList)])
                attrs = Clusters.PowerSource.Attributes
                values = battery_read[3][Clusters.PowerSource]
                from chip.clusters.Types import NullValue
                percent = values[attrs.BatPercentRemaining]
                charge = values[attrs.BatChargeState]
                result['remote_battery'] = {
                    'percent_half_units': None if percent is NullValue or percent == NullValue else int(percent),
                    'charging': int(charge), 'features': values[attrs.FeatureMap],
                    'description': values[attrs.Description], 'status': int(values[attrs.Status])}
                assert 3 in battery_read[0][Clusters.Descriptor][Clusters.Descriptor.Attributes.PartsList]
                assert values[attrs.EndpointList] == []
                assert values[attrs.FeatureMap] & 6 == 6
                assert values[attrs.Description] == 'Remote battery'
                assert any(t.deviceType == 0x11 for t in battery_read[3][Clusters.Descriptor][Clusters.Descriptor.Attributes.DeviceTypeList])
                if args.expect_battery is not None:
                    assert result['remote_battery']['percent_half_units'] == (None if args.expect_battery == -1 else args.expect_battery * 2)
                if args.expect_charging is not None:
                    assert int(charge) == {'unknown':0,'yes':1,'no':3}[args.expect_charging]
            player = read[1][Clusters.Descriptor]
            speaker = read[2][Clusters.Descriptor]
            result["player_types"] = [entry.deviceType for entry in player[Clusters.Descriptor.Attributes.DeviceTypeList]]
            result["speaker_types"] = [entry.deviceType for entry in speaker[Clusters.Descriptor.Attributes.DeviceTypeList]]
            result["player_clusters"] = player[Clusters.Descriptor.Attributes.ServerList]
            result["speaker_clusters"] = speaker[Clusters.Descriptor.Attributes.ServerList]
            result["inputs"] = [{"index": entry.index, "name": entry.name}
                for entry in read[1][Clusters.MediaInput][Clusters.MediaInput.Attributes.InputList]]
            result["current_input"] = read[1][Clusters.MediaInput][Clusters.MediaInput.Attributes.CurrentInput]
            result["playback_state"] = int(read[1][Clusters.MediaPlayback][Clusters.MediaPlayback.Attributes.CurrentState])
            result["parts"] = read[0][Clusters.Descriptor][Clusters.Descriptor.Attributes.PartsList]
            assert sorted(result["parts"]) == [1, 2, 3], result["parts"]
            result["player_power"] = read[1][Clusters.OnOff][Clusters.OnOff.Attributes.OnOff]
            result["keypad_features"] = read[1][Clusters.KeypadInput][Clusters.KeypadInput.Attributes.FeatureMap]
            result["speaker_volume"] = str(read[2][Clusters.LevelControl][Clusters.LevelControl.Attributes.CurrentLevel])
            speaker_on_off = read[2][Clusters.OnOff][Clusters.OnOff.Attributes.OnOff]
            result["speaker_on_off"] = str(speaker_on_off)
            if isinstance(speaker_on_off, bool):
                result["speaker_muted"] = not speaker_on_off
            result["fabric_count"] = read[0][Clusters.OperationalCredentials][Clusters.OperationalCredentials.Attributes.CommissionedFabrics]
            result["software_version_number"] = read[0][Clusters.BasicInformation][Clusters.BasicInformation.Attributes.SoftwareVersion]
            result["software_version"] = read[0][Clusters.BasicInformation][Clusters.BasicInformation.Attributes.SoftwareVersionString]
            result["address"] = controller.GetAddressAndPort(args.node)
            assert 0x28 in result["player_types"] and 0x22 in result["speaker_types"]
            assert {6, 1286, 1287, 1289}.issubset(result["player_clusters"])
            assert {6, 8}.issubset(result["speaker_clusters"])
            assert result["keypad_features"] == 7
            assert isinstance(result["player_power"], bool)
            assert isinstance(result["current_input"], int)
            if not args.pase_only:
                assert result["fabric_count"] > 0
            if args.expected_version:
                assert result["software_version"] == args.expected_version
            if args.expect_input_count is not None:
                assert len(result["inputs"]) == args.expect_input_count
            if args.expect_unknown_audio:
                from chip.clusters.Types import NullValue
                from chip.clusters.Attribute import ValueDecodeFailure
                assert read[2][Clusters.LevelControl][Clusters.LevelControl.Attributes.CurrentLevel] == NullValue
                assert isinstance(read[2][Clusters.OnOff][Clusters.OnOff.Attributes.OnOff], ValueDecodeFailure)
                result["unknown_audio_preserved"] = True
            if args.expect_volume is not None:
                assert read[2][Clusters.LevelControl][Clusters.LevelControl.Attributes.CurrentLevel] == args.expect_volume
                result["observed_volume_checked"] = True
            if args.expect_muted is not None:
                # Speaker OnOff represents unmuted, the inverse of CEC's mute bit.
                assert read[2][Clusters.OnOff][Clusters.OnOff.Attributes.OnOff] is (args.expect_muted == "no")
                result["observed_mute_checked"] = True
            try:
                await controller.SendCommand(args.node, 2, Clusters.LevelControl.Commands.MoveToLevel(
                    level=100, transitionTime=0, optionsMask=0, optionsOverride=0))
                raise AssertionError("Absolute volume incorrectly accepted")
            except InteractionModelError as err:
                assert err.status == Status.UnsupportedCommand
                result["absolute_volume_rejected"] = True
            response = await controller.SendCommand(args.node, 1, Clusters.KeypadInput.Commands.SendKey(keyCode=64))
            assert response.status == 1
            result["unsupported_key_rejected"] = True
            await controller.SendCommand(args.node, 2, Clusters.LevelControl.Commands.Step(
                stepMode=0, stepSize=0, transitionTime=0, optionsMask=0, optionsOverride=0))
            result["zero_step_accepted"] = True
            if args.exercise_power_inputs:
                assert result["inputs"], "Configure a test activity before exercising input selection"

                async def wait_state(attribute, expected):
                    for _ in range(20):
                        values = await controller.ReadAttribute(args.node, [(1, attribute)])
                        cluster = Clusters.OnOff if attribute is Clusters.OnOff.Attributes.OnOff else Clusters.MediaInput
                        if values[1][cluster][attribute] == expected:
                            return
                        await asyncio.sleep(0.5)
                    raise AssertionError("Hub state did not follow accepted Matter command")

                try:
                    await controller.SendCommand(args.node, 1, Clusters.OnOff.Commands.On())
                    await wait_state(Clusters.OnOff.Attributes.OnOff, True)
                    for entry in result["inputs"]:
                        await controller.SendCommand(args.node, 1,
                            Clusters.MediaInput.Commands.SelectInput(index=entry["index"]))
                        await wait_state(Clusters.MediaInput.Attributes.CurrentInput, entry["index"])
                    await controller.SendCommand(args.node, 1, Clusters.OnOff.Commands.Off())
                    await wait_state(Clusters.OnOff.Attributes.OnOff, False)
                    result["power_input_controls_exercised"] = True
                finally:
                    if any(entry["index"] == result["current_input"] for entry in result["inputs"]):
                        await controller.SendCommand(args.node, 1,
                            Clusters.MediaInput.Commands.SelectInput(index=result["current_input"]))
                    original_power = Clusters.OnOff.Commands.On if result["player_power"] else Clusters.OnOff.Commands.Off
                    await controller.SendCommand(args.node, 1, original_power())
                    await wait_state(Clusters.OnOff.Attributes.OnOff, result["player_power"])
            if args.exercise_controls:
                for command in (Clusters.MediaPlayback.Commands.Play,
                                Clusters.MediaPlayback.Commands.Pause,
                                Clusters.MediaPlayback.Commands.Stop,
                                Clusters.MediaPlayback.Commands.Previous,
                                Clusters.MediaPlayback.Commands.Next):
                    response = await controller.SendCommand(args.node, 1, command())
                    assert response.status == 0
                    await asyncio.sleep(0.3)
                for key in [0, 1, 2, 3, 4, 9, 11, 13, *range(0x20, 0x2a)]:
                    response = await controller.SendCommand(args.node, 1,
                        Clusters.KeypadInput.Commands.SendKey(keyCode=key))
                    assert response.status == 0
                    await asyncio.sleep(0.3)
                for mode in [0, 1]:
                    await controller.SendCommand(args.node, 2, Clusters.LevelControl.Commands.Step(
                        stepMode=mode, stepSize=1, transitionTime=0, optionsMask=0, optionsOverride=0))
                    await asyncio.sleep(0.3)
                result["controls_exercised"] = True
            if args.speaker_mute is not None:
                command = Clusters.OnOff.Commands.Off if args.speaker_mute == "muted" else Clusters.OnOff.Commands.On
                await controller.SendCommand(args.node, 2, command())
                result["speaker_mute_command_accepted"] = args.speaker_mute
            if args.volume_step is not None:
                await controller.SendCommand(args.node, 2, Clusters.LevelControl.Commands.Step(
                    stepMode=0 if args.volume_step >= 0 else 1, stepSize=abs(args.volume_step),
                    transitionTime=0, optionsMask=0, optionsOverride=0))
                result["volume_step_command_accepted"] = args.volume_step
                result["command_monotonic_ns"] = time.monotonic_ns()
                if args.command_marker:
                    args.command_marker.write_text(json.dumps({
                        "accepted_steps": args.volume_step,
                        "monotonic_ns": result["command_monotonic_ns"],
                    }))
            result["passed"] = True

        asyncio.run(run())
    except Exception as err:
        result["error"] = type(err).__name__ + ": " + str(err)
    finally:
        (args.storage / "result.json").write_text(json.dumps(result, indent=2, default=str))
        if controller is not None:
            controller.Shutdown()
        if manager is not None:
            manager.Shutdown()
        if stack is not None:
            stack.Shutdown()
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
