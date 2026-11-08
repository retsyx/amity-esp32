"""Per-DUT HTTPS connection lifetime for authenticated shipping UI requests."""

import http.client
import io
import logging
import math
import socket
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlsplit, urlunsplit

from .trace import event


class BufferedResponse(io.BytesIO):
    """A completely received HTTP body, independent of the reusable socket."""

    def __init__(self, body, response, url):
        super().__init__(body)
        self.status = self.code = response.status
        self.reason = response.reason
        self.headers = response.headers
        self.will_close = response.will_close
        self.url = url

    def getheader(self, name, default=None):
        return self.headers.get(name, default)

    def info(self):
        return self.headers

    def geturl(self):
        return self.url


class _DeadlineReader(io.RawIOBase):
    def __init__(self, raw, transport):
        self._raw, self._transport = raw, transport

    def readable(self):
        return True

    def readinto(self, buffer):
        self._transport.budget()
        count = self._raw.readinto(buffer)
        self._transport.budget()
        return count

    def close(self):
        try:
            self._raw.close()
        finally:
            super().close()


class _DeadlineSocket:
    """Charge every HTTP send/receive, including header reads, to one budget."""

    def __init__(self, socket, clock, deadline):
        self._socket, self._clock, self.deadline = socket, clock, deadline

    def __getattr__(self, name):
        return getattr(self._socket, name)

    def budget(self):
        remaining = self.deadline - self._clock()
        if remaining <= 0:
            raise TimeoutError("HTTP request deadline expired")
        self._socket.settimeout(remaining)

    def sendall(self, data):
        self.budget()
        self._socket.sendall(data)
        self.budget()

    def makefile(self, mode):
        if mode != "rb":
            raise ValueError("Expected binary HTTP response reads")
        # Keep the real socket file's ownership reference: http.client closes
        # its socket before reading a Connection: close response body.
        raw = self._socket.makefile(mode, buffering=0)
        return io.BufferedReader(_DeadlineReader(raw, self))


class HTTPSOpener:
    """Reuse one origin/context without ever resending a failed request.

    Complete bodies are buffered before releasing the transport lock. Healthy
    sockets remain reusable across idle periods; a server close or any I/O
    failure drops them. A later explicit request can connect, with its own budget.
    """

    def __init__(self, origin, context, *, dut=None, connection_factory=None, clock=None):
        parsed = urlsplit(origin)
        if (parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password
                or parsed.path not in ("", "/") or parsed.query or parsed.fragment):
            raise ValueError("Use one HTTPS origin without embedded credentials")
        self._host, self._port = parsed.hostname, parsed.port or 443
        self._context = context
        self._dut = dut
        self._factory = connection_factory or http.client.HTTPSConnection
        self._clock = clock or time.monotonic
        self._connection = None
        self._lock = threading.Lock()

    def _drop(self, reason):
        connection, self._connection = self._connection, None
        if connection is not None:
            try:
                connection.close()
            finally:
                event("http.connection.closed", level=logging.DEBUG, dut=self._dut, reason=reason)

    def close(self):
        """Release an idle connection; a later explicit request can reopen it."""
        with self._lock:
            self._drop("cleanup")

    def _budget(self, deadline):
        remaining = deadline - self._clock()
        if remaining <= 0:
            raise TimeoutError("HTTP request deadline expired")
        if self._connection is not None:
            self._connection.timeout = remaining
            if self._connection.sock is not None:
                if isinstance(self._connection.sock, _DeadlineSocket):
                    self._connection.sock.deadline = deadline
                self._connection.sock.settimeout(remaining)
        return remaining

    def open(self, request, timeout=15):
        if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                or not math.isfinite(timeout) or timeout <= 0):
            raise ValueError("HTTP timeout must be positive and finite")
        parsed = urlsplit(request.full_url)
        if (parsed.scheme != "https" or parsed.hostname != self._host or (parsed.port or 443) != self._port
                or parsed.username or parsed.password or parsed.fragment):
            raise ValueError("HTTP request must use this DUT's HTTPS origin")
        deadline = self._clock() + timeout
        if not self._lock.acquire(timeout=timeout):
            raise TimeoutError("HTTP transport deadline expired")
        response = None
        try:
            self._budget(deadline)
            if self._connection is None:
                remaining = self._budget(deadline)
                self._connection = self._factory(self._host, self._port, timeout=remaining, context=self._context)
                self._connection.connect()
                self._connection.sock = _DeadlineSocket(self._connection.sock, self._clock, deadline)
                self._budget(deadline)
                event("http.connection.opened", level=logging.DEBUG, dut=self._dut)
            headers = dict(request.header_items())
            target = urlunsplit(("", "", parsed.path or "/", parsed.query, ""))
            self._budget(deadline)
            self._connection.request(request.get_method(), target, body=request.data, headers=headers)
            self._budget(deadline)
            response = self._connection.getresponse()
            self._budget(deadline)
            body = bytearray()
            while True:
                self._budget(deadline)
                chunk = response.read1(65536)
                self._budget(deadline)
                if not chunk:
                    break
                body.extend(chunk)
            # HTTPResponse.read1 returns EOF for a truncated Content-Length
            # body; read() would have raised IncompleteRead. Preserve that fence.
            if response.length not in (None, 0):
                raise http.client.IncompleteRead(bytes(body), response.length)
            result = BufferedResponse(bytes(body), response, request.full_url)
            close_requested = any(key.lower() == "connection" and value.lower() == "close"
                                  for key, value in headers.items())
            response.close()
            response = None
            if result.will_close or close_requested:
                self._drop("server_close" if result.will_close else "requested_close")
            if not 200 <= result.status < 300:
                # A redirect is surfaced as an HTTP status. Never forward a
                # bearer token or a mutation to another origin or replay it.
                raise HTTPError(request.full_url, result.status, result.reason, result.headers, result)
            return result
        except BaseException:
            if response is not None:
                try:
                    response.close()
                except Exception as error:
                    event("http.response.close.failed", level=logging.ERROR, dut=self._dut,
                          error_type=type(error).__name__)
            try:
                self._drop("request_failed")
            except Exception as error:
                event("http.connection.close.failed", level=logging.ERROR, dut=self._dut,
                      error_type=type(error).__name__)
            raise
        finally:
            self._lock.release()

    def disconnect_after(self, request, after_send, *, timeout=15):
        """Send once, observe admission externally, then close before the reply.

        ``after_send`` must be bounded and must not call this opener. This is
        used to exercise an HTTP client's disappearance during a bounded key
        hold. No response is consumed, and the mutation is never replayed.
        """
        if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                or not math.isfinite(timeout) or timeout <= 0):
            raise ValueError("HTTP timeout must be positive and finite")
        if not callable(after_send):
            raise TypeError("An external admission observer is required")
        parsed = urlsplit(request.full_url)
        if (parsed.scheme != "https" or parsed.hostname != self._host or (parsed.port or 443) != self._port
                or parsed.username or parsed.password or parsed.fragment):
            raise ValueError("HTTP request must use this DUT's HTTPS origin")
        deadline = self._clock() + timeout
        if not self._lock.acquire(timeout=timeout):
            raise TimeoutError("HTTP transport deadline expired")
        try:
            self._budget(deadline)
            if self._connection is None:
                remaining = self._budget(deadline)
                self._connection = self._factory(self._host, self._port, timeout=remaining, context=self._context)
                self._connection.connect()
                self._connection.sock = _DeadlineSocket(self._connection.sock, self._clock, deadline)
                self._budget(deadline)
                event("http.connection.opened", level=logging.DEBUG, dut=self._dut)
            headers = dict(request.header_items())
            target = urlunsplit(("", "", parsed.path or "/", parsed.query, ""))
            self._connection.request(request.get_method(), target, body=request.data, headers=headers)
            self._budget(deadline)
            result = after_send()
            self._budget(deadline)
            return result
        finally:
            try:
                if self._connection is not None and self._connection.sock is not None:
                    try:
                        self._connection.sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        # An already closed peer still represents a lost client.
                        pass
                self._drop("client_disconnect")
            finally:
                self._lock.release()
