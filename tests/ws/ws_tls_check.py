#!/usr/bin/env python3
"""End-to-end wss:// check for Mercury's built-in web server.

Proves the TLS path actually carries the protocol rather than merely linking:
handshake under TLS with the Sec-WebSocket-Accept digest verified, the status
and spectrum frames the UI lives on, a command and its reply, ping/pong, and
the closing handshake.

    python3 tests/ws/ws_tls_check.py <port> <ca-cert.pem>

Worth having as a build step because the failure it catches is silent: TLS is
detected with pkg-config, so a build without OpenSSL still produces a working
mercury that just refuses wss:// at startup.

Copyright (C) 2026 Rhizomatica
Author: Rafael Diniz <rafael@riseup.net>
SPDX-License-Identifier: GPL-3.0-or-later
"""

import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    port = int(sys.argv[1])
    cafile = sys.argv[2]

    ctx = ssl.create_default_context(cafile=cafile)
    raw = socket.create_connection(("127.0.0.1", port), timeout=15)
    s = ctx.wrap_socket(raw, server_hostname="localhost")
    print("TLS up: %s %s" % (s.version(), s.cipher()[0]))

    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /websocket HTTP/1.1\r\nHost: localhost\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % key).encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            print("server closed during the handshake")
            return 1
        buf += d

    head, rest = buf.split(b"\r\n\r\n", 1)
    head = head.decode(errors="replace")
    if "101 Switching Protocols" not in head:
        print("no upgrade:\n%s" % head)
        return 1

    want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    got = None
    for line in head.split("\r\n"):
        if line.lower().startswith("sec-websocket-accept"):
            got = line.split(":", 1)[1].strip()
    if got != want:
        print("accept key mismatch: %r != %r" % (got, want))
        return 1
    print("handshake over TLS OK, accept key verified")

    state = {"buf": rest}

    def need(n):
        while len(state["buf"]) < n:
            d = s.recv(65536)
            if not d:
                raise EOFError
            state["buf"] += d

    def frame():
        need(2)
        b0, b1 = state["buf"][0], state["buf"][1]
        if b1 & 0x80:
            raise AssertionError("server must not mask its frames")
        n = b1 & 0x7F
        off = 2
        if n == 126:
            need(4)
            n = struct.unpack("!H", state["buf"][2:4])[0]
            off = 4
        elif n == 127:
            need(10)
            n = struct.unpack("!Q", state["buf"][2:10])[0]
            off = 10
        need(off + n)
        payload = state["buf"][off:off + n]
        state["buf"] = state["buf"][off + n:]
        return b0 & 0x0F, payload

    def client_frame(op, payload):
        m = os.urandom(4)
        n = len(payload)
        if n < 126:
            h = struct.pack("!BB", 0x80 | op, 0x80 | n)
        elif n <= 0xFFFF:
            h = struct.pack("!BBH", 0x80 | op, 0x80 | 126, n)
        else:
            h = struct.pack("!BBQ", 0x80 | op, 0x80 | 127, n)
        return h + m + bytes(payload[i] ^ m[i & 3] for i in range(n))

    texts = binaries = 0
    kinds = set()
    deadline = time.time() + 6
    while time.time() < deadline and (texts == 0 or binaries == 0):
        op, payload = frame()
        if op == 1:
            texts += 1
            try:
                kinds.add(json.loads(payload.decode())["type"])
            except Exception:
                pass
        elif op == 2:
            binaries += 1

    print("over TLS: %d text frames, %d binary frames, types %s"
          % (texts, binaries, sorted(kinds)))
    if texts == 0:
        print("no status frames arrived over TLS")
        return 1

    s.sendall(client_frame(1, json.dumps({"command": "set_waterfall",
                                          "value": "1"}).encode()))
    ack = None
    deadline = time.time() + 6
    while time.time() < deadline:
        op, payload = frame()
        if op == 1 and b'"status"' in payload:
            ack = payload.decode()
            break
    if ack is None:
        print("no reply to a command over TLS")
        return 1
    print("command ack over TLS: %s" % ack)

    s.sendall(client_frame(0x9, b"tls-ping"))
    deadline = time.time() + 6
    pong = None
    while time.time() < deadline:
        op, payload = frame()
        if op == 0xA:
            pong = payload
            break
    if pong != b"tls-ping":
        print("ping was not answered over TLS (got %r)" % pong)
        return 1
    print("pong over TLS OK")

    s.sendall(client_frame(0x8, struct.pack("!H", 1000)))
    deadline = time.time() + 6
    while time.time() < deadline:
        op, payload = frame()
        if op == 0x8:
            code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
            print("close echoed over TLS, code %d" % code)
            print("wss OK")
            return 0

    print("close was not echoed over TLS")
    return 1


if __name__ == "__main__":
    sys.exit(main())
