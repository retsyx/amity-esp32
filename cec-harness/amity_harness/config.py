"""Runner configuration kept outside functional test bodies."""

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass, field
import math
from pathlib import Path
from types import MappingProxyType
from typing import Callable, Mapping

from .cec import CecSession


@dataclass(frozen=True)
class RunConfig:
    main_dut: str = "amity-dut"
    credentials: Mapping[str, Path] = field(default_factory=dict)
    default_credentials: Path | None = None
    urls: Mapping[str, str] = field(default_factory=dict)
    default_url: str | None = None
    cafile: Path | None = None
    insecure: bool = False
    wiring_path: Path | None = None
    session_factory: Callable = CecSession
    gpio_factory: Callable | None = None
    keyboard_factory: Callable | None = None
    enrollment_factory: Callable | None = None
    admin_origins: dict[str, str] = field(default_factory=dict, init=False, repr=False, compare=False)
    enrollment_timeout: float = 240
    timeout: float = 180
    cec_timeout: float = 5
    firmware: Path | None = None
    firmware_timeout: float = 2700

    def __post_init__(self):
        if not isinstance(self.main_dut, str) or not self.main_dut:
            raise ValueError("The main DUT must have a nonempty bench name")
        for description, value in (("Run timeout", self.timeout), ("CEC timeout", self.cec_timeout),
                                   ("Firmware timeout", self.firmware_timeout),
                                   ("Enrollment timeout", self.enrollment_timeout)):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                raise ValueError(f"{description} must be a positive finite number")
        if not callable(self.session_factory):
            raise ValueError("session_factory must be callable")
        if self.gpio_factory is not None and not callable(self.gpio_factory):
            raise ValueError("gpio_factory must be callable or None")
        if self.keyboard_factory is not None and not callable(self.keyboard_factory):
            raise ValueError("keyboard_factory must be callable or None")
        if self.enrollment_factory is not None and not callable(self.enrollment_factory):
            raise ValueError("enrollment_factory must be callable or None")
        if not isinstance(self.insecure, bool):
            raise ValueError("insecure must be a boolean")
        if self.cafile is not None and self.insecure:
            raise ValueError("Choose a CA certificate or insecure TLS, not both")
        credentials = dict(self.credentials)
        urls = dict(self.urls)
        if any(not isinstance(name, str) or not name for name in (*credentials, *urls)):
            raise ValueError("DUT configuration names must be nonempty strings")
        if any(not isinstance(url, str) or not url for url in urls.values()):
            raise ValueError("DUT URL overrides must be nonempty strings")
        if self.default_url is not None and (not isinstance(self.default_url, str) or not self.default_url):
            raise ValueError("The default DUT URL must be a nonempty string")
        object.__setattr__(self, "credentials", MappingProxyType({name: Path(path) for name, path in credentials.items()}))
        object.__setattr__(self, "urls", MappingProxyType(urls))
        for name in ("default_credentials", "cafile", "wiring_path", "firmware"):
            value = getattr(self, name)
            if value is not None:
                object.__setattr__(self, name, Path(value))

    def credentials_for(self, name):
        return self.credentials.get(name, self.default_credentials)

    def url_for(self, name):
        return self.urls.get(name, self.default_url)


_config = ContextVar("amity_harness_run_config", default=RunConfig())
_testbeds = ContextVar("amity_harness_testbed_collector", default=None)


def current_config():
    """Read on the runner/test thread, before creating worker threads."""
    return _config.get()


@contextmanager
def use_config(config):
    if not isinstance(config, RunConfig):
        raise TypeError("Expected RunConfig")
    token = _config.set(config)
    try:
        yield config
    finally:
        _config.reset(token)


@contextmanager
def track_testbeds():
    """Collect beds constructed by a test even if its function returns None."""
    beds = []
    token = _testbeds.set(beds)
    try:
        yield beds
    finally:
        _testbeds.reset(token)


def register_testbed(bed):
    beds = _testbeds.get()
    if beds is not None:
        beds.append(bed)
