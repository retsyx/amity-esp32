"""Plain HTTP on port 80 redirects to the same URL on the main's HTTPS UI."""

import http.client
import json
from urllib.parse import urlsplit
from urllib.request import Request

from amity_harness import DUT
from amity_harness.trace import event

HTTP_PORT = 80
READY = "/api/ready"
# Each request target must come back unchanged apart from its scheme.
REQUESTS = (("GET", "/?from=http", None),
            ("GET", READY, None),
            ("POST", "/api/login", b"{}"))


def _plain_http(host, method, target, body, *, timeout=15):
    connection = http.client.HTTPConnection(host, HTTP_PORT, timeout=timeout)
    try:
        connection.request(method, target, body=body,
                           headers={"Content-Type": "application/json"} if body is not None else {})
        response = connection.getresponse()
        return response.status, response.getheader("Location"), response.will_close, response.read()
    finally:
        connection.close()


def test_http_redirect():
    dut = DUT("amity-dut")
    host = urlsplit(dut.url).hostname
    authority = f"[{host}]" if ":" in host else host
    locations = {}
    for method, target, body in REQUESTS:
        status, location, closes, content = _plain_http(host, method, target, body)
        expected = f"https://{authority}{target}"
        if status != 307 or location != expected:
            raise AssertionError(f"HTTP {method} {target} returned {status} to {location!r}, not 307 to {expected!r}")
        if not closes or content:
            raise AssertionError(f"HTTP {method} {target} redirect must be empty and close its connection")
        event("http.redirect", dut=dut.name, method=method, target=target, location=location)
        locations[target] = location
    # Following the redirect reaches this DUT's own HTTPS UI.
    with dut._opener.open(Request(locations[READY], headers={"Accept": "application/json"}),
                          timeout=15) as response:
        ready = json.load(response)
    if ready.get("device_id") != dut.identity.device_id:
        raise AssertionError("The HTTPS redirect target is not this DUT's administration UI")
    event("http.redirect.verified", dut=dut.name, requests=len(REQUESTS))
