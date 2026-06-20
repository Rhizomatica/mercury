#!/usr/bin/env python3
# drive.py — drive an ARQ file transfer between the two loopsim mercury
# instances (A=caller TESTA on :8300, B=listener TESTB on :8400) and report
# delivered bytes / wall time / integrity.  Run run_loopsim.sh first.
#
# Env: PAYLOAD=NNNN (bytes, default 5120)  TIMEOUT=SS (drain timeout, default 240)
#
# Copyright (C) 2026 Rhizomatica  /  SPDX-License-Identifier: GPL-3.0-or-later
import socket, time, sys, os

A_CTRL, A_DATA = 8300, 8301
B_CTRL, B_DATA = 8400, 8401
PAYLOAD = int(os.environ.get("PAYLOAD", "5120"))
TIMEOUT = float(os.environ.get("TIMEOUT", "240"))

def ctl(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(5)
    return s

def send(s, cmd):
    s.sendall((cmd + "\r").encode())
    time.sleep(0.2)
    try:
        return s.recv(256).decode(errors="replace").strip()
    except socket.timeout:
        return ""

B = ctl(B_CTRL); print("B MYCALL:", send(B, "MYCALL TESTB"), "| LISTEN:", send(B, "LISTEN ON"))
A = ctl(A_CTRL); print("A MYCALL:", send(A, "MYCALL TESTA"))
dA = socket.create_connection(("127.0.0.1", A_DATA), timeout=5)
dB = socket.create_connection(("127.0.0.1", B_DATA), timeout=5); dB.settimeout(1.0)

print("A CONNECT:", send(A, "CONNECT TESTA TESTB"))
A.settimeout(1.0); t0 = time.time(); connected = False
while time.time() - t0 < 90:
    try:
        line = A.recv(256).decode(errors="replace")
        if "CONNECTED" in line:
            connected = True; print("== CONNECTED in %.1fs ==" % (time.time() - t0)); break
        if "DISCONNECTED" in line:
            print("!! DISCONNECTED before connect"); break
    except socket.timeout:
        pass
if not connected:
    print("no CONNECTED in 90s"); sys.exit(1)

payload = (b"MERCURY-LOOPSIM-0123456789ABCDEF" * (PAYLOAD // 32 + 1))[:PAYLOAD]
dA.sendall(payload); print("sent %d bytes, draining..." % len(payload))
rx = b""; t0 = time.time()
while len(rx) < len(payload) and time.time() - t0 < TIMEOUT:
    try:
        b = dB.recv(4096)
        if b:
            rx += b; print("  B rx %d/%d" % (len(rx), len(payload)))
    except socket.timeout:
        pass
ok = rx == payload
secs = time.time() - t0
bps = (len(rx) * 8 / secs) if secs > 0 else 0
print("=== RESULT: %d/%d bytes in %.1fs (%.0f bps) match=%s ===" % (len(rx), len(payload), secs, bps, ok))
sys.exit(0 if ok and len(rx) == len(payload) else 1)
