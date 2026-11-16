"""Host checks for the HTTP-to-HTTPS redirect scenario's assertions, not wire qualification."""

import io
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from functional_tests import http_redirect


class Bench:
    """Answer port-80 requests as the firmware does unless a fault is selected."""

    def __init__(self, *, status=307, location=None, keep_alive=False, device_id="main-id"):
        self.status, self.location, self.keep_alive, self.device_id = status, location, keep_alive, device_id
        self.requests, self.followed = [], []
        bench = self

        class Connection:
            def __init__(self, host, port, timeout):
                self.host, self.port, self.timeout = host, port, timeout

            def request(self, method, target, body=None, headers=None):
                bench.requests.append((self.host, self.port, method, target, body))
                self.target = target

            def getresponse(self):
                location = bench.location(self.target) if bench.location else "https://amity-dut.local" + self.target
                return SimpleNamespace(status=bench.status, will_close=not bench.keep_alive,
                                       getheader=lambda name: location if name == "Location" else None,
                                       read=lambda: b"")

            def close(self):
                pass

        class Opener:
            def open(self, request, timeout):
                bench.followed.append(request.full_url)
                return io.BytesIO(json.dumps({"device_id": bench.device_id, "ready": True}).encode())

        self.connection = Connection
        self.dut = SimpleNamespace(name="amity-dut", url="https://amity-dut.local",
                                   identity=SimpleNamespace(device_id="main-id"), _opener=Opener())

    def run(self):
        with patch.object(http_redirect, "DUT", return_value=self.dut), \
                patch.object(http_redirect.http.client, "HTTPConnection", self.connection):
            http_redirect.test_http_redirect()


class HTTPRedirectScenarioTests(unittest.TestCase):
    def test_every_method_and_target_redirects_unchanged_and_reaches_the_main(self):
        bench = Bench()
        bench.run()
        self.assertEqual(bench.requests, [("amity-dut.local", 80, method, target, body)
                                          for method, target, body in http_redirect.REQUESTS])
        self.assertEqual(bench.followed, ["https://amity-dut.local/api/ready"])

    def test_changed_url_status_or_kept_connection_fails_before_following(self):
        faults = ({"location": lambda target: "https://amity-dut.local/"},
                  {"location": lambda target: "https://amity.local" + target},
                  {"status": 301}, {"status": 200}, {"keep_alive": True})
        for fault in faults:
            with self.subTest(fault=fault):
                bench = Bench(**fault)
                with self.assertRaises(AssertionError):
                    bench.run()
                self.assertEqual(len(bench.requests), 1)
                self.assertEqual(bench.followed, [])

    def test_redirect_to_another_device_fails(self):
        bench = Bench(device_id="other-id")
        with self.assertRaisesRegex(AssertionError, "not this DUT"):
            bench.run()


if __name__ == "__main__":
    unittest.main()
