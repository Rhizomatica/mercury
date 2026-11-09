#!/usr/bin/env python3
# carrier_offset_air.py -- the air between two Mercury modems, in one process.
#
# Reads the two modems' TX FIFOs and writes their RX FIFOs, with:
#
#   * a carrier offset -- the transmitter's frequency error as the receiver
#     sees it.  The offset is the SAME operation as utils/bench_common.c's
#     bench_freq_shift_real(): the signal is made analytic, its spectrum is
#     shifted by whole FFT bins, and the result is taken back to real.  The
#     only difference is that this one streams: each block is shifted with
#     its own transform, and the phase is corrected by the block's absolute
#     position so the carrier stays continuous across block boundaries.
#   * AWGN at a noise density in dB/Hz.
#   * a HALF-DUPLEX channel.  Two stations on one HF channel cannot both
#     transmit: the one that is transmitting puts the other one into the
#     noise.  Without this the bench lies -- a callee that answers while
#     the caller is still sending is a collision, and both receivers see
#     nothing.  A real radio cannot do it either, because it is one
#     frequency.
#
# The carrier sense here is the audio itself: the modems' idle audio is
# silence and their bursts are loud, so a direction whose level is above
# the threshold is transmitting.
#
#   usage: carrier_offset_air.py <dir> [--freq HZ] [--no DBHZ] [--thresh R]
#
# The dir must hold a_tx.fifo, b_tx.fifo, a_rx.fifo and b_rx.fifo.
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import os
import select
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from carrier_offset_channel import OffsetChannel  # noqa: E402


class FifoReader:
    """A FIFO read side that reopens when the writer goes away."""

    def __init__(self, path):
        self.path = path
        self.fd = None

    def fileno(self):
        if self.fd is None:
            self.fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK)
        return self.fd

    def read(self):
        try:
            data = os.read(self.fileno(), 65536)
        except OSError:
            return b""
        if not data:
            # EOF: the writer closed (or has not opened yet).  Reopen on the
            # next poll; the modem reopens its FIFO too.
            os.close(self.fd)
            self.fd = None
        return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dir")
    ap.add_argument("--freq", type=float, default=0.0, help="carrier offset in Hz")
    ap.add_argument("--no", dest="no_dbhz", type=float, default=None,
                    help="AWGN noise density in dB/Hz (default: clean)")
    ap.add_argument("--thresh", type=float, default=0.02,
                    help="carrier-detect level in int16 units (default 0.02)")
    ap.add_argument("--fs", type=int, default=8000)
    ap.add_argument("--block", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--trace", action="store_true")
    args = ap.parse_args()

    d = args.dir
    # The write ends open when the modems open theirs for reading.
    out_a = open(os.path.join(d, "a_rx.fifo"), "wb")   # B's TX -> A's RX
    out_b = open(os.path.join(d, "b_rx.fifo"), "wb")   # A's TX -> B's RX
    readers = {"a": FifoReader(os.path.join(d, "a_tx.fifo")),
               "b": FifoReader(os.path.join(d, "b_tx.fifo"))}
    channels = {k: OffsetChannel(fs=args.fs, offset_hz=args.freq,
                                 no_dbhz=args.no_dbhz, seed=args.seed + (0 if k == "a" else 1))
                for k in ("a", "b")}
    pending = {"a": np.zeros(0, dtype=np.float64),
               "b": np.zeros(0, dtype=np.float64)}
    # The carrier estimate per direction, decaying so that a burst that has
    # ended stops masking the other station.
    level = {"a": 0.0, "b": 0.0}
    last = time.time()
    t0 = last
    moved = {"a": 0, "b": 0}
    delivered = 0

    while True:
        now = time.time()
        dt = now - last
        last = now
        decay = np.exp(-dt / 0.3)
        level["a"] *= decay
        level["b"] *= decay

        fds = []
        for k, r in readers.items():
            try:
                fds.append(r.fileno())
            except OSError:
                pass
        if fds:
            select.select(fds, [], [], 0.05)

        for k, r in readers.items():
            data = r.read()
            if data:
                arr = (np.frombuffer(data, dtype="<i4") >> 16).astype(np.float64)
                pending[k] = np.concatenate([pending[k], arr])

        for k in ("a", "b"):
            other = "b" if k == "a" else "a"
            while len(pending[k]) >= args.block:
                # PACE THE AUDIO.  A FIFO carries samples as fast as the
                # writer can put them there: a whole burst lands in the
                # reader's hands in a few milliseconds, and the modems' turn
                # timers -- seconds long -- never line up with it.  A radio's
                # air takes as long as the burst does.  So the air delivers
                # at the sample rate and waits when it is ahead.
                allowed = int((time.time() - t0) * args.fs) - delivered
                if allowed < args.block:
                    break
                x = pending[k][:args.block]
                pending[k] = pending[k][args.block:]
                rms = float(np.sqrt(np.mean(x * x)))
                level[k] = max(level[k], rms)
                if level[other] > args.thresh:
                    # The other station is transmitting: this one is in the
                    # noise, as on a shared channel.
                    y = np.zeros(args.block)
                else:
                    y = channels[k].process(x)
                v = np.clip(np.round(y), -32768.0, 32767.0).astype("<i4") << 16
                (out_a if other == "a" else out_b).write(v.tobytes())
                moved[k] += args.block
                delivered += args.block
                if args.trace:
                    print(f"air {time.time() - t0:7.3f}s: {k} -> {other} "
                          f"rms {rms:8.1f} {'MASKED' if level[other] > args.thresh else ''}",
                          file=sys.stderr, flush=True)

        for k in ("a", "b"):
            other = "b" if k == "a" else "a"
            out = out_a if other == "a" else out_b
            out.flush()

        time.sleep(0.005)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
