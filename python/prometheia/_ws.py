# SPDX-License-Identifier: GPL-2.0-or-later
"""A minimal WebSocket client (RFC 6455): what protocol v4 needs and no more.

Client side only, binary messages, no extensions and no subprotocol, over a
plain TCP socket or TLS (wss://, checked against the system's trust store).
Pings are answered; a close ends the connection.
"""
import base64
import hashlib
import os
import socket
import ssl
import struct
import urllib.parse

from ._errors import Error

_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 8, 9, 10


class WebSocket:
    def __init__(self, url, timeout=60.0, max_message=64 << 20):
        u = urllib.parse.urlsplit(url)
        if u.scheme not in ("ws", "wss"):
            raise Error("a binary server's address starts with ws:// or wss://")
        self.url = url
        self.host = u.hostname
        self.port = u.port or (443 if u.scheme == "wss" else 47190)
        self.path = u.path or "/"
        self.tls = u.scheme == "wss"
        self.timeout = timeout
        self.max_message = max_message
        self.sock = None
        self._buf = bytearray()

    def connect(self):
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
            if self.tls:
                sock = ssl.create_default_context().wrap_socket(sock, server_hostname=self.host)
        except OSError as e:
            raise Error("%s: %s" % (self.url, e)) from None
        self.sock = sock
        key = base64.b64encode(os.urandom(16)).decode()
        self._send_raw(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n"
                        % (self.path, self.host, self.port, key)).encode())
        while b"\r\n\r\n" not in self._buf:
            self._fill()
            if len(self._buf) > 16384:
                raise Error("%s: oversized upgrade response" % self.url)
        head, _, rest = bytes(self._buf).partition(b"\r\n\r\n")
        self._buf = bytearray(rest)
        lines = head.decode("latin-1").split("\r\n")
        if not lines[0].startswith("HTTP/1.1 101"):
            raise Error("%s refused the WebSocket upgrade: %s" % (self.url, lines[0]))
        want = base64.b64encode(hashlib.sha1(key.encode() + _GUID).digest()).decode()
        accept = [ln.split(":", 1)[1].strip() for ln in lines[1:]
                  if ln.lower().startswith("sec-websocket-accept:")]
        if accept != [want]:
            raise Error("%s answered the upgrade with the wrong accept key" % self.url)

    def _send_raw(self, data):
        try:
            self.sock.sendall(data)
        except OSError as e:
            raise Error("%s: %s" % (self.url, e)) from None

    def _fill(self):
        try:
            chunk = self.sock.recv(65536)
        except OSError as e:
            raise Error("%s: %s" % (self.url, e)) from None
        if not chunk:
            raise Error("%s closed the connection" % self.url)
        self._buf += chunk

    def _read(self, n):
        while len(self._buf) < n:
            self._fill()
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def _frame(self, opcode, payload):
        # A client masks every frame (RFC 6455 5.3).
        head = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head.append(0x80 | n)
        elif n < 65536:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        head += mask
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self._send_raw(bytes(head) + body)

    def send(self, message):
        self._frame(OP_BINARY, message)

    def recv(self):
        """The next binary message, whole."""
        message = bytearray()
        while True:
            b0, b1 = self._read(2)
            fin, opcode, n = b0 & 0x80, b0 & 0x0F, b1 & 0x7F
            if b1 & 0x80:
                raise Error("%s sent a masked frame" % self.url)
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            if n + len(message) > self.max_message:
                raise Error("%s sent a message larger than %d bytes" % (self.url, self.max_message))
            payload = self._read(n)
            if opcode == OP_PING:
                self._frame(OP_PONG, payload)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_CLOSE:
                self.close()
                raise Error("%s closed the connection" % self.url)
            if opcode not in (OP_BINARY, OP_CONT):
                raise Error("%s sent a text frame; protocol v4 is binary" % self.url)
            message += payload
            if fin:
                return bytes(message)

    def close(self):
        if self.sock is not None:
            try:
                self._frame(OP_CLOSE, struct.pack(">H", 1000))
            except Error:
                pass
            self.sock.close()
            self.sock = None
