#!/usr/bin/env python3
# carrier_offset_bench.py -- a real Mercury session across a carrier offset.
#
# Two real Mercury modems, their FIFO audio bridged by a channel in each
# direction, with the transmitter that far off frequency.  Nothing is
# simulated but the propagation: real modulation, real demodulation, the real
# ARQ state machine.
#
# The question: what does a session do when the carrier is off by more than
# the acquisition's capture window?  The OFDM modes capture about +-55 Hz
# (docs/OFFSET-ACQUISITION-FINDINGS.md) and the MFSK floor about +-100 Hz
# (docs/MFSK-PORT.md), so the CALL -- which goes out on DATAC16 first and
# then alternates DATAC16 and MFSK -- should still be heard at an offset
# that only MFSK can capture, and should fail when neither can.
#
#   usage: carrier_offset_bench.py [OFFSET_HZ ...]
#
# Environment: MERCURY_BIN (default ../mercury), TIMEOUT_S (default 90),
# NO_DBHZ (default -100, i.e. clean), KEEP (keep the temp dir), PORT_BASE
# (default 21000), PYTHON (default python3).
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MERCURY = os.environ.get("MERCURY_BIN", os.path.join(ROOT, "mercury"))
AIR = os.path.join(HERE, "carrier_offset_air.py")
PYTHON = os.environ.get("PYTHON", "python3")
TIMEOUT_S = float(os.environ.get("TIMEOUT_S", "90"))
NO_DBHZ = os.environ.get("NO_DBHZ", "-100")
PORT_BASE = int(os.environ.get("PORT_BASE", "21000"))


class Control:
    """A TNC control connection that stays open, so the asynchronous
    responses (CONNECTED, DISCONNECTED, ...) are seen."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.settimeout(0.3)
        self.buf = b""
        self.lines = []

    def send(self, command, wait=1.0):
        self.sock.sendall((command + "\r").encode())
        self.drain(wait)

    def drain(self, wait=1.0):
        deadline = time.time() + wait
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            self.buf += chunk
        while b"\r" in self.buf:
            line, self.buf = self.buf.split(b"\r", 1)
            text = line.decode(errors="replace").strip()
            if text:
                self.lines.append(text)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_port(port, proc, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def run_offset(offset, port_base):
    d = tempfile.mkdtemp(prefix="caroffset")
    procs = []
    logs = {}
    a_port, b_port = port_base, port_base + 100
    controls = []

    def spawn(cmd, name, stdin=None, stdout=None):
        log = open(os.path.join(d, name + ".log"), "wb")
        p = subprocess.Popen(cmd, stdin=stdin, stdout=stdout, stderr=log, cwd=ROOT)
        procs.append(p)
        logs[name] = log.name
        return p

    try:
        for f in ("a_rx", "a_tx", "b_rx", "b_tx"):
            os.mkfifo(os.path.join(d, f + ".fifo"))

        def start_mercury(name, rx, tx, port):
            return spawn([MERCURY, "-x", "fifo",
                          "-i", os.path.join(d, rx + ".fifo"),
                          "-o", os.path.join(d, tx + ".fifo"),
                          "-p", str(port), "-b", str(port + 100),
                          "-m", "1", "-C", os.path.join(d, "none.ini")],
                         name, stdout=open(os.path.join(d, name + ".log"), "wb"))

        p_a = start_mercury("A", "a_rx", "a_tx", a_port)
        p_b = start_mercury("B", "b_rx", "b_tx", b_port)

        # The air: one process for both directions, so it can model the
        # half-duplex channel (one station at a time) and apply the carrier
        # offset on the way.
        spawn([PYTHON, AIR, d, "--freq", str(offset), "--no", NO_DBHZ] + (["--trace"] if os.environ.get("TRACE") else []), "air")

        if not wait_port(a_port, p_a) or not wait_port(b_port, p_b):
            return {"offset": offset, "result": "control port never opened"}

        ca = Control(a_port)
        cb = Control(b_port)
        controls += [ca, cb]
        ca.send("MYCALL TESTA", 1.5)
        cb.send("MYCALL TESTB", 1.5)
        ca.send("LISTEN ON", 1.5)
        time.sleep(1.0)
        cb.send("CONNECT TESTB TESTA", 2.0)

        connected = False
        sn = ""
        deadline = time.time() + TIMEOUT_S
        while time.time() < deadline:
            ca.drain(0.5)
            cb.drain(0.5)
            for line in ca.lines + cb.lines:
                if line.startswith("CONNECTED"):
                    connected = True
                m = re.match(r"SN\s+(-?\d+(?:\.\d+)?)", line)
                if m:
                    sn = m.group(1)
            if connected:
                break
            time.sleep(1.0)

        # What the session ran on: the modes the modems report in their logs.
        modes = []
        for name in ("A", "B"):
            path = os.path.join(d, name + ".log")
            if not os.path.exists(path):
                continue
            with open(path, "r", errors="replace") as f:
                text = f.read()
            for m in re.finditer(r"mode=(\d+) \(([A-Z0-9]+)\)", text):
                modes.append(m.group(2))
        return {"offset": offset,
                "result": "connected" if connected else "no connection",
                "sn": sn, "mode": modes[-1] if modes else "?",
                "modes": sorted(set(modes)), "dir": d}
    finally:
        for c in controls:
            c.close()
        for p in procs:
            try:
                p.send_signal(signal.SIGINT)
            except OSError:
                pass
        time.sleep(0.5)
        for p in procs:
            try:
                p.kill()
            except OSError:
                pass
        for _, name in logs.items():
            pass
        if not os.environ.get("KEEP"):
            subprocess.run(["rm", "-rf", d])


def main():
    for path in (MERCURY, AIR):
        if not os.path.exists(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2
    offsets = [float(a) for a in sys.argv[1:]] or [0.0, 50.0, 100.0, 140.0]
    print(f"{'offset':>8}  {'result':<14} {'mode':<10} {'sn':>7}  modes seen")
    for i, off in enumerate(offsets):
        r = run_offset(off, PORT_BASE + 200 * i)
        print(f"{r['offset']:>8.1f}  {r['result']:<14} {r.get('mode',''):<10} "
              f"{r.get('sn',''):>7}  {','.join(r.get('modes', []))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
