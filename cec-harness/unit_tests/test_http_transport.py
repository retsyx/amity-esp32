"""Exercise real HTTP framing on a fake socket; no DUT or network access."""

import http.client
import io
import json
from pathlib import Path
import socket
import ssl
import sys
import unittest
from urllib.error import HTTPError
from urllib.request import Request
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from amity_harness.http_transport import HTTPSOpener
from amity_harness.trace import stdio_logging
from contextlib import redirect_stderr


class Clock:
    def __init__(self):
        self.now = 100.0
    def __call__(self):
        return self.now


class WireFile(io.RawIOBase):
    def __init__(self, data, socket):
        self.data = io.BytesIO(data)
        self.socket = socket
    def readable(self):
        return True
    def readinto(self, target):
        self.socket.factory.clock.now += self.socket.factory.read_delay
        data = self.data.read(min(len(target), self.socket.factory.fragment))
        target[:len(data)] = data
        return len(data)


class Socket:
    def __init__(self, factory):
        self.factory = factory
        self.timeouts = []
        self.closed = False
        self.shutdowns = []
        self.wire = b""
    def settimeout(self, timeout):
        self.timeouts.append(timeout)
    def sendall(self, data):
        self.factory.clock.now += self.factory.send_delay
    def makefile(self, mode, buffering=0):
        return WireFile(self.wire, self)
    def close(self):
        self.closed = True
    def shutdown(self, how):
        self.shutdowns.append(how)


class Connection:
    def __init__(self, factory, timeout):
        self.factory, self.timeout = factory, timeout
        self.sock = None
        self.requests = []
        self.closed = False
    def connect(self):
        self.factory.clock.now += self.factory.connect_delay
        self.sock = Socket(self.factory)
        self.factory.sockets.append(self.sock)
    def request(self, method, target, body=None, headers=None):
        self.requests.append((method, target, body, headers))
        self.sock.sendall(b"request")
        value = self.factory.responses.pop(0)
        if isinstance(value, BaseException):
            raise value
        getattr(self.sock, "_socket", self.sock).wire = value
    def getresponse(self):
        response = http.client.HTTPResponse(self.sock)
        response.begin()
        if response.will_close:
            self.close()
        return response
    def close(self):
        self.closed = True
        if self.sock is not None:
            self.sock.close()
            self.sock = None


class Factory:
    def __init__(self, *responses):
        self.responses = list(responses)
        self.clock = Clock()
        self.connections, self.sockets, self.arguments = [], [], []
        self.connect_delay = self.read_delay = self.send_delay = 0
        self.fragment = 8192
    def __call__(self, host, port, *, timeout, context):
        self.arguments.append((host, port, timeout, context))
        result = Connection(self, timeout)
        self.connections.append(result)
        return result
    def opener(self, context=None):
        return HTTPSOpener("https://amity.local", context or ssl.create_default_context(),
                           clock=self.clock, connection_factory=self, dut="amity-dut")


def body(value, *, status="200 OK", close=False):
    raw = json.dumps(value).encode()
    return (f"HTTP/1.1 {status}\r\nContent-Length: {len(raw)}\r\n"
            + ("Connection: close\r\n" if close else "") + "\r\n").encode() + raw


def request(data=None, path="/api/system"):
    return Request("https://amity.local" + path, data=data,
                   headers={"Authorization": "Bearer PRIVATE_TOKEN", "Accept": "application/json"})


class TransportTests(unittest.TestCase):
    def test_authenticated_requests_reuse_one_connection_and_exact_context(self):
        factory = Factory(body({"ready": True}), body({"ready": False}))
        context = ssl.create_default_context()
        opener = factory.opener(context)
        with opener.open(request(), timeout=3) as first:
            self.assertEqual(json.load(first), {"ready": True})
        factory.clock.now += 0.4
        with opener.open(request(), timeout=8) as second:
            self.assertFalse(json.load(second)["ready"])
        self.assertEqual(len(factory.connections), 1)
        self.assertIs(factory.arguments[0][3], context)
        self.assertEqual(factory.connections[0].requests[1][3]["Authorization"], "Bearer PRIVATE_TOKEN")
        self.assertAlmostEqual(factory.connections[0].timeout, 8)
        opener.close()
        self.assertTrue(factory.sockets[0].closed)

    def test_body_is_complete_before_next_request_even_without_caller_read(self):
        factory = Factory(body({"first": True}), body({"second": True}))
        opener = factory.opener()
        first = opener.open(request())
        with opener.open(request()) as second:
            self.assertEqual(json.load(second), {"second": True})
        self.assertEqual(json.load(first), {"first": True})
        self.assertEqual(len(factory.connections), 1)
        first.close();opener.close()

    def test_chunked_body_and_trailers_use_standard_HTTP_parser(self):
        wire = b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{"a"\r\n3\r\n:1}\r\n0\r\nX-Result: done\r\n\r\n'
        factory = Factory(wire, body({"next": True}))
        factory.fragment = 2
        opener = factory.opener()
        with opener.open(request()) as response:
            self.assertEqual(json.load(response), {"a": 1})
        with opener.open(request()) as response:
            self.assertTrue(json.load(response)["next"])
        self.assertEqual(len(factory.connections), 1)
        opener.close()

    def test_truncated_content_length_closes_without_resend(self):
        factory = Factory(b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc')
        opener = factory.opener()
        with self.assertRaises(http.client.IncompleteRead) as failure:
            opener.open(request())
        self.assertEqual(failure.exception.partial, b"abc")
        self.assertEqual(failure.exception.expected, 7)
        self.assertEqual(len(factory.connections[0].requests), 1)
        self.assertTrue(factory.sockets[0].closed)

    def test_missing_final_chunk_closes_without_resend(self):
        factory = Factory(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n')
        opener = factory.opener()
        with self.assertRaises(http.client.IncompleteRead):
            opener.open(request())
        self.assertTrue(factory.sockets[0].closed)
        self.assertEqual(len(factory.connections[0].requests), 1)

    def test_long_idle_reuses_one_socket_without_extra_requests_or_handshakes(self):
        factory = Factory(*(body({"n": number}) for number in range(5)))
        factory.connect_delay = 1.5
        opener = factory.opener()
        opener.open(request(), timeout=3).close()
        for idle in (2, 10, 60, 3600):
            factory.clock.now += idle
            before = factory.clock.now
            opener.open(request(), timeout=1).close()
            self.assertEqual(factory.clock.now, before)
            self.assertEqual(len(factory.connections), 1)
            self.assertEqual(len(factory.sockets), 1)
            self.assertFalse(factory.sockets[0].closed)
        self.assertEqual(len(factory.connections[0].requests), 5)
        self.assertEqual(factory.responses, [])
        self.assertTrue(all(not any(name.lower() == "connection" for name in headers)
                            for _, _, _, headers in factory.connections[0].requests))
        opener.close()
        self.assertTrue(factory.sockets[0].closed)
        self.assertIsNone(opener._connection)

    def test_server_declared_close_opens_fresh_before_later_request(self):
        factory = Factory(body({"n": 1}, close=True), body({"n": 2}), body({"n": 3}))
        opener = factory.opener()
        with opener.open(request()) as response:
            self.assertEqual(json.load(response)["n"], 1)
        self.assertTrue(factory.sockets[0].closed)
        opener.open(request()).close()
        factory.clock.now += 3600
        opener.open(request()).close()
        self.assertEqual(len(factory.connections), 2)
        self.assertFalse(factory.sockets[1].closed)
        opener.close()

    def test_stale_connection_error_never_resends_mutation_or_GET(self):
        for failure in (ConnectionResetError("PRIVATE_ERROR"), b""):
            for payload in (None, b'{"request_id":"same"}'):
                with self.subTest(payload=payload, failure=type(failure).__name__):
                    factory = Factory(body({}), failure, body({"later": True}))
                    opener = factory.opener()
                    opener.open(request()).close()
                    factory.clock.now += 60
                    error = ConnectionResetError if isinstance(failure, BaseException) else http.client.RemoteDisconnected
                    with self.assertRaises(error):
                        opener.open(request(payload))
                    self.assertEqual(len(factory.connections), 1)
                    self.assertEqual(len(factory.connections[0].requests), 2)
                    self.assertEqual(factory.connections[0].requests[1][2], payload)
                    self.assertEqual(len(factory.responses), 1)
                    self.assertTrue(factory.sockets[0].closed)
                    # A later caller request may connect, without replaying the failed one.
                    opener.open(request()).close()
                    self.assertEqual(len(factory.connections), 2)
                    self.assertEqual(len(factory.connections[1].requests), 1)
                    opener.close()

    def test_server_closed_idle_connection_is_replaced_before_sending(self):
        for closed in (False, True):
            for payload in (None, b'{"request_id":"once"}'):
                with self.subTest(closed=closed, payload=payload):
                    factory = Factory(body({}), body({"later": True}))
                    opener = factory.opener()
                    opener.open(request()).close()
                    local, server = socket.socketpair()
                    self.addCleanup(local.close)
                    self.addCleanup(server.close)
                    factory.sockets[0].fileno = local.fileno
                    if closed:
                        server.close()  # The firmware purged this idle socket.
                    opener.open(request(payload)).close()
                    self.assertEqual(len(factory.connections), 2 if closed else 1)
                    self.assertEqual(sum(len(connection.requests) for connection in factory.connections), 2)
                    self.assertEqual(factory.connections[-1].requests[-1][2], payload)
                    self.assertEqual(factory.sockets[0].closed, closed)
                    opener.close()

    def test_idle_connection_is_reused_before_explicit_client_disconnect(self):
        factory = Factory(body({}), body({"unused": "hold response"}), body({"later": True}))
        opener = factory.opener()
        opener.open(request()).close()
        connection = factory.connections[0]
        factory.clock.now += 60

        def admitted():
            self.assertEqual(len(factory.connections), 1)
            self.assertEqual(len(connection.requests), 2)
            self.assertEqual(connection.requests[1][:3], ("POST", "/api/key", b'{"key":65,"duration_ms":3000}'))
            self.assertFalse(factory.sockets[0].closed)
            return "physical press"

        with patch.object(connection, "getresponse", side_effect=AssertionError("The disconnected hold must not read its response")):
            self.assertEqual(opener.disconnect_after(request(b'{"key":65,"duration_ms":3000}', "/api/key"), admitted),
                             "physical press")
        self.assertEqual(factory.sockets[0].shutdowns, [socket.SHUT_RDWR])
        self.assertTrue(factory.sockets[0].closed)
        self.assertIsNone(opener._connection)
        self.assertFalse(opener._lock.locked())
        opener.open(request()).close()
        self.assertEqual(len(factory.connections), 2)
        self.assertEqual(len(factory.connections[1].requests), 1)
        opener.close()

    def test_redirects_and_foreign_origin_never_receive_bearer_or_mutation(self):
        factory = Factory(b'HTTP/1.1 307 Redirect\r\nLocation: https://other.local/api/steal\r\nContent-Length: 0\r\n\r\n')
        opener = factory.opener()
        with self.assertRaises(HTTPError) as failure:
            opener.open(request(b"PRIVATE_PASSWORD"))
        self.assertEqual(failure.exception.code, 307)
        self.assertEqual(len(factory.connections), 1)
        self.assertEqual(len(factory.connections[0].requests), 1)
        failure.exception.close()
        for origin in ("http://amity.local", "https://other.local", "https://amity.local:444", "https://user:password@amity.local"):
            with self.subTest(origin=origin), self.assertRaises(ValueError):
                opener.open(Request(origin + "/api/system"))
        self.assertEqual(len(factory.connections), 1)

    def test_HTTP_status_body_is_readable_but_not_logged(self):
        factory = Factory(body({"error": "PRIVATE_BODY"}, status="401 Unauthorized"))
        opener = factory.opener()
        output = io.StringIO()
        with redirect_stderr(output), stdio_logging("DEBUG"):
            with self.assertRaises(HTTPError) as failure:
                opener.open(request())
        self.assertEqual(failure.exception.code, 401)
        self.assertEqual(json.load(failure.exception), {"error": "PRIVATE_BODY"})
        failure.exception.close()
        self.assertNotIn("PRIVATE", output.getvalue())
        self.assertNotIn("amity.local", output.getvalue())
        self.assertTrue(factory.sockets[0].closed)

    def test_total_deadline_includes_connect_and_slow_header_progress(self):
        factory = Factory(body({}))
        factory.connect_delay = 2
        opener = factory.opener()
        with self.assertRaises(TimeoutError):
            opener.open(request(), timeout=1)
        self.assertTrue(factory.sockets[0].closed)
        self.assertFalse(factory.connections[0].requests)
        factory = Factory(body({}))
        factory.fragment, factory.read_delay = 1, 0.2
        opener = factory.opener()
        with self.assertRaises(TimeoutError):
            opener.open(request(), timeout=1)
        self.assertLessEqual(factory.clock.now, 101.21)
        self.assertTrue(factory.sockets[0].closed)

    def test_transport_lock_wait_cannot_extend_timeout(self):
        factory = Factory(body({}))
        opener = factory.opener()
        opener._lock.acquire()
        try:
            with self.assertRaises(TimeoutError):
                opener.open(request(), timeout=0.001)
        finally:
            opener._lock.release()
        self.assertFalse(factory.connections)


if __name__ == "__main__":
    unittest.main()
