#!/usr/bin/env python3
# drive.py — drive an ARQ file transfer between the two loopsim mercury
# instances (A=caller TESTA on :8300, B=listener TESTB on :8400) and report
# delivered bytes / wall time / integrity.  Run run_loopsim.sh first.
#
# Env: PAYLOAD=NNNN (bytes, default 5120)  TIMEOUT=SS (drain timeout, default 240)
#
# Every run ends the ARQ session it opened and does not return until the
# over-the-air teardown has actually finished on both stations, so runs can be
# repeated back to back against the same two instances.  Two things made that
# fail before, both producing a regular "0 bytes / too many bytes" alternation
# that looked like a wedge and was only this script:
#
#   - Status was matched by substring, and "DISCONNECTED" contains "CONNECTED".
#     A stale teardown notice arriving during the next connect was taken as a
#     successful connect, and the payload went into a dead link.  Status is
#     now parsed as whole \r-terminated lines and matched exactly.
#
#   - DISCONNECTED is not an "idle" signal on its own.  Per the VARA TNC
#     convention mercury sends it IMMEDIATELY when the DISCONNECT command
#     arrives (arq.c, ARQ_CMD_DISCONNECT), and again when the air-side teardown
#     completes.  Taking the first as "done" started the next CONNECT while
#     both ends were still DISCONNECTING, where mercury drops it silently.  The
#     caller now waits for its second DISCONNECTED and the listener for its
#     own, with the sockets still open.
#
# Exit status: 0 only if the payload arrived intact AND the session tore down
# cleanly; a transfer that passed but left the link half-open exits 2, because
# whatever runs next against these instances would be measuring that.
#
# Copyright (C) 2026 Rhizomatica  /  SPDX-License-Identifier: GPL-3.0-or-later
import socket, time, sys, os

A_CTRL, A_DATA = 8300, 8301
B_CTRL, B_DATA = 8400, 8401
PAYLOAD = int(os.environ.get("PAYLOAD", "5120"))
TIMEOUT = float(os.environ.get("TIMEOUT", "240"))
TEARDOWN_TIMEOUT = 90.0   # DISCONNECT retries + guard intervals, with margin


class Ctl:
    """A control-port connection that yields complete status lines."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = ""

    def send(self, cmd):
        self.s.sendall((cmd + "\r").encode())

    def lines(self, secs):
        """Yield status lines received within `secs` seconds."""
        end = time.time() + secs
        while True:
            while "\r" in self.buf or "\n" in self.buf:
                cut = min(i for i in (self.buf.find("\r"), self.buf.find("\n")) if i >= 0)
                line, self.buf = self.buf[:cut].strip(), self.buf[cut + 1:]
                if line:
                    yield line
            left = end - time.time()
            if left <= 0:
                return
            self.s.settimeout(min(1.0, left))
            try:
                chunk = self.s.recv(512)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.buf += chunk.decode(errors="replace")

    def expect(self, secs, pred):
        """First line satisfying pred within secs, else None."""
        for line in self.lines(secs):
            if pred(line):
                return line
        return None


def is_connected(line):
    return line.startswith("CONNECTED ")


def is_disconnected(line):
    return line == "DISCONNECTED"


def disconnect(A, B):
    """End the session and wait until the air-side teardown finished on both."""
    A.send("DISCONNECT")
    # 1st DISCONNECTED on A: the command was accepted (sent before teardown).
    ack = A.expect(10, is_disconnected)
    # Listener: its only DISCONNECTED comes when its own teardown completes.
    b_done = B.expect(TEARDOWN_TIMEOUT, is_disconnected)
    # Caller: the 2nd DISCONNECTED comes when its teardown completes.
    a_done = A.expect(TEARDOWN_TIMEOUT, is_disconnected) if ack else None
    clean = bool(ack and a_done and b_done)
    print("== teardown: A %s, B %s ==" % ("done" if a_done else "NOT CONFIRMED",
                                          "done" if b_done else "NOT CONFIRMED"))
    if not clean:
        print("!! WARNING: session did not tear down cleanly; a run started "
              "now would measure the leftover session, not a fresh one")
    return clean


B = Ctl(B_CTRL); B.send("MYCALL TESTB"); B.send("LISTEN ON")
A = Ctl(A_CTRL); A.send("MYCALL TESTA")
time.sleep(0.3)
dA = socket.create_connection(("127.0.0.1", A_DATA), timeout=5)
dB = socket.create_connection(("127.0.0.1", B_DATA), timeout=5); dB.settimeout(1.0)

t0 = time.time()
A.send("CONNECT TESTA TESTB")
line = A.expect(90, lambda l: is_connected(l) or is_disconnected(l))
if not line or not is_connected(line):
    print("no CONNECTED in 90s (%s)" % (line or "nothing"))
    disconnect(A, B)
    sys.exit(1)
print("== %s in %.1fs ==" % (line, time.time() - t0))

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
clean = disconnect(A, B)
sys.exit(0 if ok and clean else (2 if ok else 1))
