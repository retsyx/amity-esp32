#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Temporary same-channel Pi AP, with an independent NetworkManager rollback.

Default is read-only networking preflight plus project-local credential/state
files. --activate requires coordinated exclusive use of the authorized Pi.
Run as root, in a detached process or through the recovery serial console.
Network profiles are volatile; the existing uplink profile is never updated.
--ap-only temporarily replaces the uplink with an AP on the same interface;
start it through a separately verified serial connection because SSH will drop.

Depends on the project's dbus-next wheel. AP credentials, status, PID, DHCP
leases and logs are written only below --directory, which must be in esp32.
NetworkManager still maintains its normal operating state and system logs.

NetworkManager API documentation:
https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html
"""

import argparse
import asyncio
import contextlib
import copy
import ipaddress
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import time
import uuid

from dbus_next import BusType, Variant
from dbus_next.aio import MessageBus

NM = "org.freedesktop.NetworkManager"
BASE = "/org/freedesktop/NetworkManager"
PROPS = "org.freedesktop.DBus.Properties"
DEVICE = NM + ".Device"
WIFI = DEVICE + ".Wireless"
ACTIVE = NM + ".Connection.Active"
PROFILE = NM + ".Settings.Connection"
AP = NM + ".AccessPoint"


def write_private(path, value):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w") as output:
        json.dump(value, output, indent=2)
        output.write("\n")


def read_private(path):
    if path.stat().st_mode & 0o077:
        raise RuntimeError("Credential file must have mode 0600")
    return json.loads(path.read_text())


async def main(args):
    if os.geteuid() != 0:
        raise RuntimeError("Run with sudo for NetworkManager and AP operations")
    directory = args.directory.resolve()
    if "esp32" not in directory.parts:
        raise RuntimeError("The work directory must be below the esp32 project")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    if args.ready_file:
        args.ready_file = args.ready_file.resolve()
        if "esp32" not in args.ready_file.parts or args.ready_file.exists():
            raise RuntimeError("The fresh readiness gate must be an absent file inside esp32")
    credentials = read_private(args.uplink_credentials)
    ap_credentials_path = directory / "ap-credentials.json"
    if ap_credentials_path.exists():
        ap_credentials = read_private(ap_credentials_path)
    else:
        ap_credentials = {"ssid": "Amity-Test-" + secrets.token_hex(3),
                          "password": secrets.token_urlsafe(24)}
        write_private(ap_credentials_path, ap_credentials)
    state = {"stage": "preflight", "pid": os.getpid(), "activated": False,
             "interface": args.ap_interface, "gateway": str(args.gateway.ip),
             "credentials_file": str(ap_credentials_path)}
    state_path = directory / "status.json"

    def stage(value, **details):
        state.update(stage=value, updated=time.time(), **details)
        write_private(state_path, state)
        print(json.dumps({"stage": value, **details}), flush=True)

    if args.wait_file:
        args.wait_file = args.wait_file.resolve()
        if "esp32" not in args.wait_file.parts or args.wait_file.exists():
            raise RuntimeError("The activation trigger must be an absent file inside esp32")
        stage("waiting-for-trigger", wait_seconds=args.wait_seconds)
        deadline = time.monotonic() + args.wait_seconds
        while not args.wait_file.exists():
            if time.monotonic() >= deadline:
                stage("trigger-expired")
                return
            await asyncio.sleep(.05)

    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    proxies = {}

    async def obj(path):
        if path not in proxies:
            proxies[path] = bus.get_proxy_object(NM, path, await bus.introspect(NM, path))
        return proxies[path]

    async def get(path, interface, prop):
        return (await (await obj(path)).get_interface(PROPS).call_get(interface, prop)).value

    manager = (await obj(BASE)).get_interface(NM)
    wlan = await manager.call_get_device_by_ip_iface(args.uplink_interface)
    wireless = (await obj(wlan)).get_interface(WIFI)
    if await get(wlan, DEVICE, "State") != 100:
        raise RuntimeError("The original uplink must be connected before creating a checkpoint")
    if not (await get(wlan, WIFI, "WirelessCapabilities")) & 0x40:
        raise RuntimeError("The adapter does not advertise AP mode")
    original_active = await get(wlan, DEVICE, "ActiveConnection")
    original_profile = await get(original_active, ACTIVE, "Connection")
    original_ap = await get(original_active, ACTIVE, "SpecificObject")
    original_uuid = await get(original_active, ACTIVE, "Uuid")
    original_settings = await (await obj(original_profile)).get_interface(PROFILE).call_get_settings()
    original_ssid = bytes(original_settings["802-11-wireless"]["ssid"].value)
    if original_ssid != credentials["ssid"].encode():
        raise RuntimeError("Approved uplink credentials do not match the current uplink profile")
    if not args.ap_only and Path("/sys/class/net", args.ap_interface).exists():
        raise RuntimeError("The test AP interface already exists; refusing to alter it")
    if await get(BASE, NM, "Checkpoints"):
        raise RuntimeError("An existing network checkpoint must be resolved first")
    if args.refresh_scan and not args.ap_only:
        previous_scan = await get(wlan, WIFI, "LastScan")
        await wireless.call_request_scan({"ssids": Variant("aay", [original_ssid])})
        for _ in range(100):
            if await get(wlan, WIFI, "LastScan") != previous_scan:
                break
            await asyncio.sleep(.2)
        else:
            raise RuntimeError("The directed Wi-Fi scan did not complete")
    strength, target_ap, target_bssid = None, None, None
    if not args.ap_only:
        matches = []
        for path in await wireless.call_get_access_points():
            if await get(path, AP, "Frequency") != 2412:
                continue
            if bytes(await get(path, AP, "Ssid")) == original_ssid:
                matches.append((await get(path, AP, "Strength"), path, await get(path, AP, "HwAddress")))
        if not matches:
            raise RuntimeError("No cached matching channel 1 AP; run a coordinated Wi-Fi scan first")
        strength, target_ap, target_bssid = max(matches)
    state.update(original_uuid=original_uuid, original_profile=original_profile,
                 original_ap=original_ap, original_frequency=await get(original_ap, AP, "Frequency"),
                 uplink_interface=args.uplink_interface, selected_bssid=target_bssid,
                 selected_strength_percent=strength, selected_channel=1, ap_only=args.ap_only)
    # Catch routing conflicts before adding an address to the new interface.
    routes = json.loads(subprocess.check_output(["/usr/sbin/ip", "-j", "-4", "route", "show"], text=True))
    for route in routes:
        destination = route.get("dst", "default")
        if destination != "default" and ipaddress.ip_network(destination, strict=False).overlaps(args.gateway.network):
            raise RuntimeError("The test subnet overlaps an existing IPv4 route")
    stage("prepared", selected_channel=1, selected_strength_percent=strength)
    if not args.activate:
        bus.disconnect()
        return

    checkpoint = None
    ap_active = None
    vif_created = False
    dnsmasq = None
    dns_log = None
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    volatile = {"persist": Variant("s", "volatile"), "bind-activation": Variant("s", "dbus-client")}

    async def wait_connected(path, expected_active, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = await get(path, DEVICE, "State")
            if value == 100 and await get(path, DEVICE, "ActiveConnection") == expected_active:
                return
            if value == 120:
                raise RuntimeError("NetworkManager connection failed")
            await asyncio.sleep(.25)
        raise RuntimeError("NetworkManager activation timed out")

    try:
        # DISCONNECT_NEW_DEVICES (0x4) includes the AP if this process dies.
        # Do not use DELETE_NEW_CONNECTIONS: it would affect unrelated profiles.
        # NM requires the all-device snapshot for DISCONNECT_NEW_DEVICES.
        # Existing interfaces are only snapshotted; their profiles are unchanged.
        checkpoint = await manager.call_checkpoint_create([], args.seconds, 0x4)
        stage("checkpoint-created", checkpoint=checkpoint, rollback_seconds=args.seconds)
        if args.ap_only:
            ap_device = wlan
        else:
            uplink = copy.deepcopy(original_settings)
            settings = uplink["connection"]
            settings.update({"id": Variant("s", "amity-test-uplink-" + secrets.token_hex(3)),
                             "uuid": Variant("s", str(uuid.uuid4())),
                             "autoconnect": Variant("b", False),
                             "interface-name": Variant("s", args.uplink_interface)})
            settings.pop("timestamp", None)
            wifi = uplink["802-11-wireless"]
            wifi.update({"band": Variant("s", "bg"), "channel": Variant("u", 1),
                         "bssid": Variant("ay", bytes.fromhex(target_bssid.replace(":", "")))})
            security = uplink.setdefault("802-11-wireless-security", {})
            security.update({"key-mgmt": Variant("s", "wpa-psk"),
                             "psk": Variant("s", credentials["password"]), "psk-flags": Variant("u", 0)})
            _profile, uplink_active, _result = await manager.call_add_and_activate_connection2(uplink, wlan, target_ap, volatile)
            await wait_connected(wlan, uplink_active)
            active_ap = await get(wlan, WIFI, "ActiveAccessPoint")
            if await get(active_ap, AP, "Frequency") != 2412:
                raise RuntimeError("Uplink did not switch to channel 1")
            stage("uplink-on-channel-1")
            subprocess.run(["/usr/sbin/iw", "dev", args.uplink_interface, "interface", "add",
                            args.ap_interface, "type", "__ap"], check=True, capture_output=True)
            vif_created = True
            ap_device = None
            for _ in range(40):
                try:
                    ap_device = await manager.call_get_device_by_ip_iface(args.ap_interface)
                    break
                except Exception:
                    await asyncio.sleep(.25)
            if ap_device is None:
                raise RuntimeError("NetworkManager did not detect the AP interface")
            await (await obj(ap_device)).get_interface(PROPS).call_set(DEVICE, "Managed", Variant("b", True))
        # The Set call returns before NM attaches the supplicant. A newly
        # managed virtual interface is briefly UNAVAILABLE (20), and cannot
        # accept a connection until it reaches DISCONNECTED (30).
        for _ in range(80):
            if await get(ap_device, DEVICE, "State") >= 30:
                break
            await asyncio.sleep(.25)
        else:
            reason = await get(ap_device, DEVICE, "StateReason")
            raise RuntimeError("AP interface stayed unavailable: " + str(reason))
        profile = {
            "connection": {"id": Variant("s", "amity-test-ap-" + secrets.token_hex(3)),
                           "uuid": Variant("s", str(uuid.uuid4())),
                           "type": Variant("s", "802-11-wireless"),
                           "interface-name": Variant("s", args.ap_interface),
                           "autoconnect": Variant("b", False)},
            "802-11-wireless": {"ssid": Variant("ay", ap_credentials["ssid"].encode()),
                                  "mode": Variant("s", "ap"), "band": Variant("s", "bg"),
                                  "channel": Variant("u", 1)},
            "802-11-wireless-security": {"key-mgmt": Variant("s", "wpa-psk"),
                                           "psk": Variant("s", ap_credentials["password"]),
                                           "proto": Variant("as", ["rsn"]),
                                           "pairwise": Variant("as", ["ccmp"]),
                                           "group": Variant("as", ["ccmp"])},
            "ipv4": {"method": Variant("s", "manual"), "never-default": Variant("b", True),
                     "address-data": Variant("aa{sv}", [{"address": Variant("s", str(args.gateway.ip)),
                                                           "prefix": Variant("u", args.gateway.network.prefixlen)}])},
            "ipv6": {"method": Variant("s", "link-local"), "never-default": Variant("b", True)},
        }
        _profile, ap_active, _result = await manager.call_add_and_activate_connection2(profile, ap_device, "/", volatile)
        await wait_connected(ap_device, ap_active)
        first, last = args.gateway.network.network_address + 10, args.gateway.network.network_address + 30
        dns_log = open(directory / "dnsmasq.log", "a")
        dnsmasq = subprocess.Popen(["/usr/sbin/dnsmasq", "--keep-in-foreground", "--conf-file=",
            "--user=root", "--group=root", "--no-resolv", "--no-hosts", "--port=0",
            "--interface=" + args.ap_interface, "--bind-interfaces", "--except-interface=lo",
            "--dhcp-authoritative", f"--dhcp-range={first},{last},{args.gateway.network.netmask},10m",
            "--dhcp-option=3," + str(args.gateway.ip), "--dhcp-option=6",
            "--dhcp-leasefile=" + str(directory / "dhcp.leases"),
            "--pid-file=" + str(directory / "dnsmasq.pid"), "--log-facility=-", "--log-dhcp"],
            stdout=dns_log, stderr=subprocess.STDOUT)
        await asyncio.sleep(.5)
        if dnsmasq.poll() is not None:
            raise RuntimeError("Project-local DHCP server failed; inspect dnsmasq.log")
        for _ in range(30):
            interfaces = json.loads(subprocess.check_output(
                ["/usr/sbin/ip", "-j", "address", "show", args.ap_interface], text=True))
            addresses = [a for interface in interfaces for a in interface["addr_info"]]
            ipv4 = any(a["family"] == "inet" and a["local"] == str(args.gateway.ip) for a in addresses)
            ipv6 = any(a["family"] == "inet6" and a["scope"] == "link" and
                       not a.get("tentative", False) for a in addresses)
            if ipv4 and ipv6:
                break
            await asyncio.sleep(.1)
        else:
            raise RuntimeError("AP IPv4/IPv6 addresses did not become ready")
        state["activated"] = True
        stage("ready", ap_interface=args.ap_interface, gateway=str(args.gateway.ip))
        if args.ready_file:
            # Release a coordinated Matter gate directly, avoiding an SSH
            # round-trip during its bounded secure-session failsafe window.
            descriptor = os.open(args.ready_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w") as output:
                output.write("AP IPv4 and IPv6 ready\n")
        deadline = time.monotonic() + args.seconds
        while not stop.is_set() and time.monotonic() < deadline:
            if checkpoint not in await get(BASE, NM, "Checkpoints"):
                break
            if dnsmasq.poll() is not None:
                raise RuntimeError("DHCP server stopped unexpectedly")
            if not Path("/sys/class/net", args.ap_interface).exists():
                raise RuntimeError("AP interface disappeared")
            if (await get(ap_device, DEVICE, "State") != 100 or
                    await get(ap_device, DEVICE, "ActiveConnection") != ap_active):
                raise RuntimeError("AP connection stopped unexpectedly")
            state["verified_at"] = time.time()
            write_private(state_path, state)
            try:
                await asyncio.wait_for(stop.wait(), 2)
            except asyncio.TimeoutError:
                pass
    except Exception as error:
        stage("failed", error=type(error).__name__ + ": " + str(error))
        raise
    finally:
        if dnsmasq is not None and dnsmasq.poll() is None:
            dnsmasq.terminate()
            try:
                dnsmasq.wait(timeout=3)
            except subprocess.TimeoutExpired:
                dnsmasq.kill()
                dnsmasq.wait()
        if dns_log:
            dns_log.close()
        if ap_active:
            with contextlib.suppress(Exception):
                await manager.call_deactivate_connection(ap_active)
        if vif_created:
            subprocess.run(["/usr/sbin/iw", "dev", args.ap_interface, "del"], capture_output=True)
        restored = checkpoint is None  # No network operation preceded checkpoint creation.
        if checkpoint:
            with contextlib.suppress(Exception):
                await manager.call_checkpoint_rollback(checkpoint)
            # A bounded explicit fallback also handles a checkpoint that already
            # expired. It activates the original profile without modifying it.
            try:
                # A driver reset replaces NM's device object along with the VIF.
                wlan = await manager.call_get_device_by_ip_iface(args.uplink_interface)
                current = await get(wlan, DEVICE, "ActiveConnection")
                restored = (current != "/" and await get(wlan, DEVICE, "State") == 100 and
                            await get(current, ACTIVE, "Uuid") == original_uuid)
                if not restored:
                    restored_active = await manager.call_activate_connection(original_profile, wlan, original_ap)
                    await wait_connected(wlan, restored_active)
                    current = await get(wlan, DEVICE, "ActiveConnection")
                    restored = await get(current, ACTIVE, "Uuid") == original_uuid
            except Exception:
                with contextlib.suppress(Exception):
                    restored_active = await manager.call_activate_connection(original_profile, wlan, "/")
                    await wait_connected(wlan, restored_active)
                    current = await get(wlan, DEVICE, "ActiveConnection")
                    restored = await get(current, ACTIVE, "Uuid") == original_uuid
            with contextlib.suppress(Exception):
                await manager.call_checkpoint_destroy(checkpoint)
        state["activated"] = False
        stage("restored" if restored else "restore-unconfirmed", original_connection_restored=restored)
        bus.disconnect()


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--uplink-credentials", type=Path, required=True)
    parser.add_argument("--uplink-interface", default="wlan0")
    parser.add_argument("--ap-interface", default="amityap0")
    parser.add_argument("--gateway", type=ipaddress.IPv4Interface, default=ipaddress.IPv4Interface("10.42.23.1/24"))
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--activate", action="store_true")
    parser.add_argument("--ap-only", action="store_true", help="replace the uplink with an AP; requires serial recovery")
    parser.add_argument("--refresh-scan", action="store_true", help="wait for a private directed uplink scan")
    parser.add_argument("--ready-file", type=Path, help="create a fresh project-local gate only after IPv4/IPv6 readiness")
    parser.add_argument("--wait-file", type=Path, help="wait for a fresh project-local marker before networking preflight/activation")
    parser.add_argument("--wait-seconds", type=int, default=300)
    arguments = parser.parse_args()
    if arguments.ap_only:
        arguments.ap_interface = arguments.uplink_interface
    if not 120 <= arguments.seconds <= 3600 or arguments.gateway.network.prefixlen != 24:
        parser.error("Use a 120–3600 second rollback timeout and a /24 test subnet")
    if not 1 <= arguments.wait_seconds <= 600:
        parser.error("Use a 1–600 second activation-trigger timeout")
    asyncio.run(main(arguments))
