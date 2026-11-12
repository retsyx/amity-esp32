"""One shipping-UI upload, observed staging, and explicit OTA reboot proofs.

No upload is replayed after an uncertain response. Configuration and fixture
ownership remain independent of boot identities; only this intentional reboot
may refresh those identities, after fresh authenticated evidence.
"""

from copy import deepcopy
from dataclasses import dataclass, field
import hashlib
import http.client
import json
from pathlib import Path
import re
import struct
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request

from .discovery import _identifier
from .dut import AmityHTTPError, _positive_seconds
from .trace import event


_HEADER = struct.Struct(">16sIIQ")
_IDENTIFIER = re.compile(r"[0-9A-Za-z-]+")
_MAX_IMAGE = 0x3cd000
_TRANSIENT = (OSError, URLError, http.client.HTTPException)
_READ_ERRORS = (*_TRANSIENT, AmityHTTPError)


def _retryable(error):
    return not isinstance(error, AmityHTTPError) or error.status in {408, 429, 500, 502, 503, 504}


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate firmware manifest field")
        result[key] = value
    return result


def _version_identifiers(text, *, prerelease):
    parts = text.split(".")
    if any(not part or _IDENTIFIER.fullmatch(part) is None
           or prerelease and part.isdigit() and len(part) > 1 and part[0] == "0" for part in parts):
        return None
    return parts


def _parse_version(text):
    if not isinstance(text, str):
        return None
    text, plus, build = text.partition("+")
    if plus and _version_identifiers(build, prerelease=False) is None:
        return None
    text, dash, pre = text.partition("-")
    pre = _version_identifiers(pre, prerelease=True) if dash else []
    core = text.split(".")
    if (pre is None or len(core) != 3
            or any(not part.isdigit() or not part.isascii() or len(part) > 1 and part[0] == "0"
                   for part in core)):
        return None
    return core, pre


def _compare_part(left, right, digits):
    if digits and len(left) != len(right):
        return -1 if len(left) < len(right) else 1
    return 0 if left == right else -1 if left < right else 1


def compare_firmware_versions(candidate, running):
    """Match firmware_version.hpp: semantic precedence ignoring build metadata.

    Returns None when either version cannot be compared.
    """
    left, right = _parse_version(candidate), _parse_version(running)
    if left is None or right is None:
        return None
    for a, b in zip(left[0], right[0]):
        if result := _compare_part(a, b, True):
            return result
    left_pre, right_pre = left[1], right[1]
    if not left_pre or not right_pre:
        return 0 if not left_pre and not right_pre else 1 if not left_pre else -1
    for a, b in zip(left_pre, right_pre):
        a_digits, b_digits = a.isdigit(), b.isdigit()
        if a_digits != b_digits:
            return -1 if a_digits else 1
        if result := _compare_part(a, b, a_digits):
            return result
    return 0 if len(left_pre) == len(right_pre) else -1 if len(left_pre) < len(right_pre) else 1


@dataclass(frozen=True)
class FirmwareBundle:
    data: bytes = field(repr=False)
    version: str
    image_bytes: int
    image_elf_sha256: str
    image_sha256: str

    @classmethod
    def load(cls, path):
        path = Path(path)
        if not _HEADER.size < path.stat().st_size <= _HEADER.size + 8192 + 64 + _MAX_IMAGE:
            raise ValueError("Select a complete bounded Amity update bundle")
        data = path.read_bytes()
        magic, manifest_size, signature_size, image_size = _HEADER.unpack(data[:_HEADER.size])
        offset = _HEADER.size + manifest_size + signature_size
        if (magic != b"AMITY-UPDATE-V1\n" or not 1 <= manifest_size <= 8192
                or signature_size not in (0, 64) or not 288 <= image_size <= _MAX_IMAGE
                or offset + image_size != len(data)):
            raise ValueError("Invalid or incomplete Amity update bundle")
        encoded = data[_HEADER.size:_HEADER.size + manifest_size]
        manifest = json.loads(encoded, object_pairs_hook=_unique)
        if (not isinstance(manifest, dict)
                or json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode() != encoded):
            raise ValueError("Use the exact canonical firmware manifest")
        image = data[offset:]
        version = manifest.get("firmware_version")
        elf, digest = manifest.get("image_elf_sha256"), manifest.get("image_sha256")
        if (not isinstance(version, str) or not version or len(version.encode()) > 31
                or not isinstance(elf, str) or re.fullmatch(r"[0-9a-f]{64}", elf) is None
                or manifest.get("image_bytes") != str(image_size)
                or digest != hashlib.sha256(image).hexdigest()
                or image[0] != 0xe9 or image[32:36] != bytes.fromhex("3254cdab")
                or image[176:208].hex() != elf
                or image[48:80].split(b"\0", 1)[0] != version.encode()):
            raise ValueError("Firmware image and release identity differ")
        return cls(data, version, image_size, elf, digest)


def saved_configuration(value):
    """Compare the whole saved document, excluding only UI revision tokens."""
    return {key: deepcopy(item) for key, item in value.items()
            if key not in {"config_revision", "presentation_revision"}}


def saved_device_settings(body):
    adapters = body.get("adapters")
    if not isinstance(adapters, list) or any(not isinstance(row, dict) for row in adapters):
        raise AssertionError("Firmware settings have no adapter list")
    result = {key: deepcopy(body.get(key)) for key in ("hostname", "hostname_revision", "settings_revision")}
    result["adapters"] = [{key: deepcopy(row.get(key)) for key in
        ("tentacle_id", "configured", "default_front_physical_address")} for row in adapters]
    return result


def _peers(system):
    rows = system.get("membership", {}).get("peers")
    if not isinstance(rows, list) or any(not isinstance(row, dict) or not _identifier(row.get("octopus_id")) for row in rows):
        raise AssertionError("Invalid firmware membership snapshot")
    peers = {row["octopus_id"]: row for row in rows}
    if len(peers) != len(rows) or any(row.get("enrolled") is not True for row in rows):
        raise AssertionError("Unexpected or provisional firmware member")
    return peers


class FirmwareUpgrade:
    """Observe all targets, then refresh only this fixture's reboot fences."""

    _cleanup_priority = -1
    _cleanup_retryable = True

    def __init__(self, main, members, bundle, *, activities=(), timeout=2700, poll_interval=0.1):
        _positive_seconds(timeout, "Firmware timeout")
        _positive_seconds(poll_interval, "Firmware polling interval")
        bed = main._testbed
        if bed is None or not bed._entered or bed._closed:
            raise RuntimeError("Upgrade firmware inside an active TestBed")
        self.main, self.members, self.bundle = main, tuple(members), bundle
        self.activities = tuple(activities)
        self.bed, self.timeout, self.poll_interval = bed, timeout, poll_interval
        self.peer_ids = {member.device_id for member in self.members}
        if len(self.peer_ids) != len(self.members) or any(member.main is not main for member in self.members):
            raise ValueError("Upgrade needs distinct members of this main")
        self.ids = self.peer_ids | {main.identity.device_id}
        self.before_system = self.after_system = self.before_config = None
        self._verified = self._attempted = self._members_rebound = False
        self._job = self._last_progress = None
        self.report = {"kind": "firmware_upgrade", "owner_ids": sorted(self.ids), "checks": []}
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append(self.report)
        bed.add_cleanup(self.close)

    def _note(self, check, **values):
        self.report["checks"].append({"check": check, **values})
        event("firmware." + check, dut=self.main.name, **values)

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Firmware upgrade did not produce all required evidence")
        return remaining

    def _get(self, path, deadline):
        while True:
            self._remaining(deadline)
            try:
                self.main.authenticate()
            except _READ_ERRORS as error:
                if not _retryable(error):
                    raise
            else:
                try:
                    return self.main.request(path, timeout=min(15, self._remaining(deadline)))
                except _READ_ERRORS as error:
                    if isinstance(error, AmityHTTPError) and error.status == 401:
                        # Reboot revokes browser sessions. Reauthenticate
                        # reads, never replay the firmware POST.
                        self.main._token = None
                    elif not _retryable(error):
                        raise
            # Final configuration/settings reads need the same restart
            # tolerance as progress reads, under the same overall deadline.
            time.sleep(min(self.poll_interval, self._remaining(deadline)))

    def _authority(self, system):
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("authority_ready") is not True or membership.get("member") is not False
                or not _identifier(system.get("boot_id")) or not _identifier(system.get("consortium_id"))
                or set(_peers(system)) != self.peer_ids):
            raise AssertionError("Firmware upgrade changed main authority or the exact member set")
        if self.before_system and system.get("consortium_id") != self.before_system["consortium_id"]:
            raise AssertionError("Firmware upgrade changed the saved system identity")

    def _settings(self, system, deadline):
        boots = {self.main.identity.device_id: system["boot_id"],
                 **{owner: row.get("boot_id") for owner, row in _peers(system).items()}}
        result = {}
        for owner in sorted(self.ids):
            while True:
                response = self._get("/api/devices/" + owner + "/settings", deadline)
                body = response.get("settings") or {}
                if response.get("status") == "ready" and not response.get("error") and body.get("boot_id") == boots[owner]:
                    result[owner] = saved_device_settings(body)
                    break
                time.sleep(min(self.poll_interval, self._remaining(deadline)))
        return result

    def _upload(self, running, deadline):
        self.main.authenticate()
        headers = {"Accept": "application/json", "Content-Type": "application/octet-stream",
                   "Authorization": "Bearer " + self.main._token}
        # Like the UI, confirm only an older or incomparable version. A
        # firmware that misjudges an upgrade as a downgrade must reject it.
        comparison = compare_firmware_versions(self.bundle.version, running)
        if comparison is None or comparison < 0:
            headers["X-Amity-Confirm-Version"] = running
        request = Request(self.main.url + "/api/firmware", data=self.bundle.data, headers=headers)
        self._attempted = True
        self._note("upload_once", image_elf_sha256=self.bundle.image_elf_sha256,
                   image_bytes=self.bundle.image_bytes, version_comparison=comparison)
        try:
            with self.main._opener.open(request, timeout=min(180, self._remaining(deadline))) as response:
                acknowledgement = json.load(response)
            if acknowledgement.get("staged") is not True:
                raise AssertionError("The firmware upload did not confirm local staging")
        except HTTPError as error:
            status = error.code
            error.close()
            raise AmityHTTPError(status, "Firmware upload was rejected") from None
        except _TRANSIENT:
            # Even if the response was lost, this POST may have succeeded.
            self._note("upload_response_unconfirmed")

    def _observe_job(self, job):
        if job.get("image_elf_sha256") != self.bundle.image_elf_sha256 or job.get("automatic") is not False:
            return False
        phase = job.get("phase")
        if phase not in {"distributing", "activating", "failed"}:
            return False
        job_id = job.get("job_id")
        if not _identifier(job_id) or self._job is not None and job_id != self._job:
            raise AssertionError("The firmware update job was replaced")
        self._job = job_id
        targets = job.get("targets", [])
        rows = {row.get("octopus_id"): row for row in targets if isinstance(row, dict)}
        if (len(rows) != len(targets) or set(rows) != self.ids
                or str(job.get("image_bytes")) != str(self.bundle.image_bytes)):
            raise AssertionError("Firmware update omitted or added a target")
        if phase == "failed" or job.get("error") or any(row.get("phase") in {"skipped", "failed"} for row in rows.values()):
            raise AssertionError("A required firmware target failed or was skipped")
        progress = (phase, tuple((owner, row.get("phase"), int(row.get("offset", -1)) * 100 // self.bundle.image_bytes)
                                 for owner, row in sorted(rows.items())))
        if progress != self._last_progress:
            self._last_progress = progress
            self._note("progress", phase=phase, targets=[{"octopus_id": owner, "phase": row.get("phase"),
                       "offset": row.get("offset")} for owner, row in sorted(rows.items())])
        if phase == "activating":
            if any(row.get("phase") not in {"staged", "activating", "complete"}
                   or str(row.get("offset")) != str(self.bundle.image_bytes) for row in rows.values()):
                raise AssertionError("Activation preceded complete staging on every Amity")
            return True
        return False

    def _prepare(self, deadline):
        if self.before_system is not None:
            raise RuntimeError("A firmware upload cannot be repeated")
        health, system = self._get("/api/setup", deadline), self._get("/api/system", deadline)
        self._authority(system)
        peers = _peers(system)
        if (health.get("boot_id") != system["boot_id"] or health.get("ready") is not True
                or system.get("ready") is not True
                or system.get("configuration_pending") is not False
                or any(row.get("ready") is not True or not _identifier(row.get("boot_id"))
                       or row.get("firmware_identity") != health.get("image_elf_sha256") for row in peers.values())):
            raise AssertionError("Every Amity must be ready on the main's firmware before upload")
        if health.get("image_elf_sha256") == self.bundle.image_elf_sha256:
            raise ValueError("The upgrade test requires a different firmware image")
        state = self._get("/api/activity-editor/status", deadline)
        if state.get("expected_boot_id") != system["boot_id"] or state.get("current_activity") != -1 or state.get("activity_settling") is not False:
            raise AssertionError("Finish activity checks in standby before firmware upload")
        self.before_system = deepcopy(system)
        self.before_config = self._get("/api/config", deadline)
        if (self.before_config.get("controller_octopus_id") != self.main.identity.device_id
                or self.before_config.get("consortium_id") != system["consortium_id"]
                or system.get("applied_config_revision") != self.before_config.get("config_revision")
                or system.get("applied_presentation_revision") != self.before_config.get("presentation_revision")):
            raise AssertionError("Firmware test configuration has not been applied")
        settings = self._settings(system, deadline)
        self.report["expected_config_sha256"] = hashlib.sha256(json.dumps(saved_configuration(self.before_config), sort_keys=True).encode()).hexdigest()
        self._note("before", main_boot=system["boot_id"], image_elf_sha256=health.get("image_elf_sha256"),
                   owner_boots={owner: row["boot_id"] for owner, row in peers.items()})
        return health, settings

    def _partial_target(self, job, owner=None, *, minimum=0):
        if self._observe_job(job):
            raise AssertionError("Firmware reached activation before the requested transfer interruption")
        if job.get("job_id") != self._job or job.get("phase") != "distributing":
            return None
        for row in job["targets"]:
            if (row["octopus_id"] in self.peer_ids and (owner is None or row["octopus_id"] == owner)
                    and row.get("phase") == "uploading"
                    and minimum < int(row.get("offset", -1)) < self.bundle.image_bytes):
                return row
        return None

    @staticmethod
    def _target_progress(row):
        return {key: row[key] for key in ("octopus_id", "phase", "offset")}

    def _original_boots(self, system, peers):
        self._authority(system)
        if (system["boot_id"] != self.before_system["boot_id"] or any(
                peer.get("boot_id") != peers[owner]["boot_id"] for owner, peer in _peers(system).items())):
            raise AssertionError("An Amity rebooted before the requested firmware interruption")

    def _unchanged_saved_state(self, system, settings, deadline):
        config = self._get("/api/config", deadline)
        if saved_configuration(config) != saved_configuration(self.before_config):
            raise AssertionError("Firmware interruption changed saved activities, equipment or settings")
        if (system.get("applied_config_revision") != config.get("config_revision")
                or system.get("applied_presentation_revision") != config.get("presentation_revision")):
            return False
        if self._settings(system, deadline) != settings:
            raise AssertionError("Firmware interruption changed an Amity's saved adapter settings or hostname")
        return True

    def interrupt_main_transfer(self):
        """Prove an abandoned distribution is safe before a separate upload.

        The manual cohort lives in RAM. Rebooting its main must leave the
        staged image inactive; it cannot count as a completed upgrade.
        """
        deadline = time.monotonic() + self.timeout
        old, settings = self._prepare(deadline)
        self._upload(old["version"], deadline)
        while True:
            self.bed.session.assert_healthy()
            row = self._partial_target(self._get("/api/updates/cohort", deadline))
            if row is not None:
                self._original_boots(self._get("/api/system", deadline), _peers(self.before_system))
                self._note("main_transfer_interrupt", main_boot=old["boot_id"],
                           targets=[self._target_progress(row)])
                self.main.usb_reboot()
                break
            time.sleep(min(self.poll_interval, self._remaining(deadline)))
        peers = _peers(self.before_system)
        while True:
            self.bed.session.assert_healthy()
            health, system = self._get("/api/setup", deadline), self._get("/api/system", deadline)
            if health.get("boot_id") != system.get("boot_id") or system.get("boot_id") == old["boot_id"]:
                time.sleep(min(self.poll_interval, self._remaining(deadline)))
                continue
            if health.get("image_elf_sha256") != old["image_elf_sha256"] or health.get("version") != old["version"]:
                raise AssertionError("Main interruption activated an incompletely distributed image")
            if system.get("membership", {}).get("authority_ready") is not True:
                time.sleep(min(self.poll_interval, self._remaining(deadline)))
                continue
            self._authority(system)
            current = _peers(system)
            for owner, peer in current.items():
                if (_identifier(peer.get("boot_id")) and peer["boot_id"] != peers[owner]["boot_id"]
                        or peer.get("firmware_identity") and peer["firmware_identity"] != old["image_elf_sha256"]):
                    raise AssertionError("A secondary activated during the interrupted main transfer")
            job, local = self._get("/api/updates/cohort", deadline), self._get("/api/updates", deadline)
            if job.get("phase") != "idle":
                raise AssertionError("The rebooted main retained or replaced the abandoned firmware cohort")
            if (local.get("phase") != "staged_image" or local.get("expected_elf_sha256") != self.bundle.image_elf_sha256
                    or local.get("running_elf_sha256") != old["image_elf_sha256"]
                    or local.get("current_boot_id") != system["boot_id"]):
                raise AssertionError("The interrupted image was not retained as an inactive staged image")
            state = self._get("/api/activity-editor/status", deadline)
            if (health.get("ready") is True and system.get("ready") is True
                    and system.get("configuration_pending") is False
                    and all(peer.get("ready") is True and peer.get("boot_id") == peers[owner]["boot_id"]
                            and peer.get("firmware_identity") == old["image_elf_sha256"]
                            for owner, peer in current.items())
                    and state.get("expected_boot_id") == system["boot_id"]
                    and state.get("current_activity") == -1 and state.get("activity_settling") is False
                    and self._unchanged_saved_state(system, settings, deadline)):
                self.after_system = deepcopy(system)
                self._note("main_transfer_aborted", main_boot=system["boot_id"],
                           image_elf_sha256=health["image_elf_sha256"],
                           owner_boots={owner: peer["boot_id"] for owner, peer in current.items()})
                return self
            time.sleep(min(self.poll_interval, self._remaining(deadline)))

    def _interrupt_secondary(self, member, row, peers, deadline):
        owner, offset = member.device_id, int(row["offset"])
        self._note("secondary_transfer_interrupt", targets=[self._target_progress(row)])
        member.candidate.usb_reboot()
        restarted = False
        while True:
            self.bed.session.assert_healthy()
            job = self._get("/api/updates/cohort", deadline)
            if self._observe_job(job) or job.get("job_id") != self._job or job.get("phase") != "distributing":
                raise AssertionError("Secondary interruption lost its original distributing firmware job")
            target = next(target for target in job["targets"] if target["octopus_id"] == owner)
            if target.get("phase") in {"queued", "uploading"} and 0 <= int(target.get("offset", -1)) < offset:
                if not restarted:
                    self._note("secondary_transfer_restarted", targets=[self._target_progress(target)])
                restarted = True
            health, system = self._get("/api/setup", deadline), self._get("/api/system", deadline)
            if health.get("boot_id") != system.get("boot_id"):
                raise AssertionError("The main rebooted during the secondary transfer interruption")
            self._authority(system)
            current = _peers(system)
            if system["boot_id"] != self.before_system["boot_id"]:
                raise AssertionError("The main rebooted during the secondary transfer interruption")
            for peer_owner, peer in current.items():
                if (peer_owner != owner and _identifier(peer.get("boot_id")) and peer["boot_id"] != peers[peer_owner]["boot_id"]
                        or peer.get("firmware_identity") and peer["firmware_identity"] != health.get("image_elf_sha256")):
                    raise AssertionError("An unrelated or premature firmware reboot occurred")
            peer = current[owner]
            if (_identifier(peer.get("boot_id")) and peer["boot_id"] != peers[owner]["boot_id"]
                    and peer.get("firmware_identity") == health.get("image_elf_sha256")):
                if not restarted:
                    raise AssertionError("Secondary transfer did not restart after its USB reboot")
                self._note("secondary_transfer_recovered", owner_boots={owner: peer["boot_id"]})
                peers[owner] = deepcopy(peer)
                return
            time.sleep(min(self.poll_interval, self._remaining(deadline)))

    def run(self, *, interrupt_secondary=None):
        if interrupt_secondary is not None and not any(interrupt_secondary is member for member in self.members):
            raise ValueError("Interrupt only a secondary in this firmware cohort")
        deadline = time.monotonic() + self.timeout
        health, settings = self._prepare(deadline)
        peers = _peers(self.before_system)
        self._upload(health["version"], deadline)
        barrier = False
        interrupted = False
        while True:
            self._remaining(deadline)
            self.bed.session.assert_healthy()
            try:
                before = self._get("/api/updates/cohort", deadline)
                health = self._get("/api/setup", deadline)
                system = self._get("/api/system", deadline)
                after = self._get("/api/updates/cohort", deadline)
            except _READ_ERRORS as error:
                if not _retryable(error):
                    raise
                time.sleep(min(self.poll_interval, self._remaining(deadline)))
                continue
            barrier = self._observe_job(before) or barrier
            barrier = self._observe_job(after) or barrier
            if health.get("boot_id") != system.get("boot_id"):
                continue  # Two reads straddled the primary reboot.
            if (system.get("octopus_id") != self.main.identity.device_id
                    or system.get("consortium_id") != self.before_system["consortium_id"]):
                raise AssertionError("Firmware upgrade changed the main or system identity")
            if system.get("membership", {}).get("authority_ready") is not True:
                time.sleep(min(self.poll_interval, self._remaining(deadline)))
                continue
            self._authority(system)
            current = _peers(system)
            if any(row.get("ready") is True and row.get("firmware_identity") != health.get("image_elf_sha256") for row in current.values()):
                raise AssertionError("A mismatched firmware secondary became operational")
            if (before.get("phase") == after.get("phase") == "distributing"
                    and before.get("job_id") == after.get("job_id") == self._job and not barrier):
                if system["boot_id"] != self.before_system["boot_id"] or any(
                    _identifier(row.get("boot_id")) and row["boot_id"] != peers[owner]["boot_id"]
                        for owner, row in current.items()):
                    raise AssertionError("An Amity rebooted before the staging barrier")
            if interrupt_secondary is not None and not interrupted:
                target = self._partial_target(after, interrupt_secondary.device_id,
                                              minimum=self.bundle.image_bytes // 4)
                if target is not None:
                    self._original_boots(system, peers)
                    self._interrupt_secondary(interrupt_secondary, target, peers, deadline)
                    interrupted = True
                    continue  # Discard the pre-interruption snapshots.
            if system["boot_id"] != self.before_system["boot_id"]:
                if not barrier:
                    raise AssertionError("Primary rebooted without an observed all-Amity staging barrier")
                if health.get("image_elf_sha256") != self.bundle.image_elf_sha256 or health.get("version") != self.bundle.version:
                    raise AssertionError("Primary booted another firmware image")
                if any(_identifier(row.get("boot_id")) and row["boot_id"] != peers[owner]["boot_id"]
                       and row.get("firmware_identity") and row["firmware_identity"] != self.bundle.image_elf_sha256
                       for owner, row in current.items()):
                    raise AssertionError("A secondary rebooted into another firmware image")
                ready = (health.get("ready") is True and system.get("ready") is True and system.get("configuration_pending") is False
                         and all(row.get("ready") is True and row.get("management_ready") is False
                                 and _identifier(row.get("boot_id")) and row["boot_id"] != peers[owner]["boot_id"]
                                 and row.get("firmware_identity") == self.bundle.image_elf_sha256 for owner, row in current.items()))
                if ready:
                    if interrupt_secondary is not None and not interrupted:
                        raise AssertionError("The requested secondary transfer interruption was missed")
                    config = self._get("/api/config", deadline)
                    if saved_configuration(config) != saved_configuration(self.before_config):
                        raise AssertionError("Firmware upgrade changed saved activities, equipment or settings")
                    if (system.get("applied_config_revision") != config.get("config_revision")
                            or system.get("applied_presentation_revision") != config.get("presentation_revision")):
                        time.sleep(min(self.poll_interval, self._remaining(deadline)))
                        continue
                    if self._settings(system, deadline) != settings:
                        raise AssertionError("Firmware upgrade changed an Amity's saved adapter settings or hostname")
                    self.after_system, self._verified = deepcopy(system), True
                    self.report.update(main_boot=system["boot_id"], owner_boots={owner: row["boot_id"] for owner, row in current.items()})
                    self._note("all_targets_healthy", image_elf_sha256=self.bundle.image_elf_sha256,
                               main_boot=system["boot_id"], owner_boots=self.report["owner_boots"])
                    return self
            time.sleep(min(self.poll_interval, self._remaining(deadline)))

    def _rebind_members(self, system):
        self._authority(system)
        peers = _peers(system)
        if any(not _identifier(row.get("boot_id")) or row.get("hostname") != member.name
               for member in self.members for row in (peers[member.device_id],)):
            raise AssertionError("Cannot refresh fixture ownership for another member")
        for member in self.members:
            member.boot, member.member_boot = system["boot_id"], peers[member.device_id]["boot_id"]
            # Preserve each original roster subset and saved baseline. Only the
            # proven boots change; later additions must not become its baseline.
            member._initial_system["boot_id"] = system["boot_id"]
            for row in member._initial_system["membership"]["peers"]:
                row["boot_id"] = peers[row["octopus_id"]]["boot_id"]

    def rebind_members(self):
        if not self._verified:
            raise RuntimeError("Verify the complete firmware upgrade before refreshing member proofs")
        self._rebind_members(self.after_system)
        self._members_rebound = True

    def rebind_activities(self, activities, scan):
        if (not self._verified or not self._members_rebound or scan is not self.main.last_scan
                or scan._session is not self.bed.session or scan._boot != self.after_system["boot_id"]):
            raise AssertionError("Activity reboot proofs require a fresh verified post-upgrade scan")
        scan.validate_owners(tuple(component for activity in activities for component in activity.components.values() if component is not None))
        document = self.main.get_configuration()
        for activity in activities:
            if (activity not in self.activities or activity.dut is not self.main
                    or [row for row in document["activities"] if row.get("activity_id") == activity.activity_id] != [activity._created_row]):
                raise AssertionError("The saved activity changed across firmware upgrade")
        for activity in activities:
            activity.boot, activity.scan = scan._boot, scan
            activity.refresh_scan(scan)
        self._note("activity_proofs_refreshed")

    def close(self):
        """Reconcile only cleanup ownership if a failed update rebooted boards.

        This does not turn a failed update into a pass or authorize new control.
        Existing helpers still remove exact owned rows and retain their saved
        baselines. Unreachable or foreign boards remain explicit cleanup errors.
        """
        if not self._attempted or self.before_system is None:
            return
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                job = self._get("/api/updates/cohort", deadline)
                system = self._get("/api/system", deadline)
                if (system.get("octopus_id") != self.main.identity.device_id
                        or system.get("consortium_id") != self.before_system["consortium_id"]):
                    raise AssertionError("Cleanup cannot adopt another main or system")
                if (job.get("phase") not in {"uploading", "distributing", "activating"}
                        and system.get("configuration_pending") is False
                        and system.get("membership", {}).get("authority_ready") is True):
                    self._authority(system)
                    break
            except _READ_ERRORS as error:
                if not _retryable(error):
                    raise
            time.sleep(min(self.poll_interval, self._remaining(deadline)))
        self._rebind_members(system)
        document = self._get("/api/activity-editor", deadline)
        errors = []
        for activity in self.activities:
            rows = [row for row in document.get("activities", []) if row.get("activity_id") == activity.activity_id]
            if rows and rows != [activity._created_row]:
                errors.append(AssertionError("Cleanup cannot adopt an edited firmware-test activity"))
            else:
                activity.boot = system["boot_id"]
        if errors:
            raise errors[0]
