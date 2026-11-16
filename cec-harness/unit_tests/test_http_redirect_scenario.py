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

    def __init__(self, *, status=308, location=None, keep_alive=False, device_id="main-id", stalled="pending"):
        self.status, self.location, self.keep_alive, self.device_id = status, location, keep_alive, device_id
        self.requests, self.followed, self.stalled_sent = [], [], []
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

        class Stalled:
            """A partial request: pending while others are served, then closed unanswered."""

            def __init__(self):
                self.blocking = True

            def __enter__(self):
                return self

            def __exit__(self, *error):
                return False

            def sendall(self, data):
                bench.stalled_sent.append(data)

            def setblocking(self, blocking):
                self.blocking = blocking

            def settimeout(self, timeout):
                self.blocking = True

            def recv(self, size):
                if not self.blocking:
                    if stalled == "closed early":
                        return b""
                    raise BlockingIOError()
                if stalled == "never closed":
                    raise TimeoutError()
                return b"H" if stalled == "answered" else b""

        class Opener:
            def open(self, request, timeout):
                bench.followed.append(request.full_url)
                return io.BytesIO(json.dumps({"device_id": bench.device_id, "ready": True}).encode())

        self.connection, self.stalled = Connection, Stalled
        self.dut = SimpleNamespace(name="amity-dut", url="https://amity-dut.local",
                                   identity=SimpleNamespace(device_id="main-id"), _opener=Opener())

    def run(self):
        with patch.object(http_redirect, "DUT", return_value=self.dut), \
                patch.object(http_redirect.http.client, "HTTPConnection", self.connection), \
                patch.object(http_redirect.socket, "create_connection", return_value=self.stalled()):
            http_redirect.test_http_redirect()


class HTTPRedirectScenarioTests(unittest.TestCase):
    def test_every_method_and_target_redirects_unchanged_and_reaches_the_main(self):
        bench = Bench()
        bench.run()
        self.assertEqual(bench.requests, [("amity-dut.local", 80, method, target, body)
                                          for method, target, body in http_redirect.REQUESTS])
        self.assertEqual(bench.stalled_sent, [b"GET / HTTP/1.1\r\nHost: "])
        self.assertEqual(bench.followed, ["https://amity-dut.local/api/ready"])

    def test_changed_url_status_or_kept_connection_fails_before_following(self):
        faults = ({"location": lambda target: "https://amity-dut.local/"},
                  {"location": lambda target: "https://amity.local" + target},
                  {"status": 307}, {"status": 301}, {"status": 200}, {"keep_alive": True})
        for fault in faults:
            with self.subTest(fault=fault):
                bench = Bench(**fault)
                with self.assertRaises(AssertionError):
                    bench.run()
                self.assertEqual(len(bench.requests), 1)
                self.assertEqual(bench.followed, [])

    def test_stalled_client_closed_early_answered_or_kept_open_fails(self):
        for stalled, message in (("closed early", "before the others"), ("answered", "received a response"),
                                 ("never closed", "did not close")):
            with self.subTest(stalled=stalled):
                bench = Bench(stalled=stalled)
                with self.assertRaisesRegex(AssertionError, message):
                    bench.run()
                self.assertEqual(len(bench.requests), len(http_redirect.REQUESTS))
                self.assertEqual(bench.followed, [])

    def test_redirect_to_another_device_fails(self):
        bench = Bench(device_id="other-id")
        with self.assertRaisesRegex(AssertionError, "not this DUT"):
            bench.run()


if __name__ == "__main__":
    unittest.main()
