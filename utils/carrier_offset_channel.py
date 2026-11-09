#!/usr/bin/env python3
"""carrier_offset_channel.py -- a streaming channel for the carrier-offset bench.

Reads raw s32le PCM (8 kHz) on stdin, applies a carrier offset -- the
transmitter's frequency error as the receiver sees it -- and AWGN, and writes
raw s32le on stdout.

The offset is the SAME operation as utils/bench_common.c's
bench_freq_shift_real(): the signal is made analytic, its spectrum is
shifted by whole FFT bins, and the result is taken back to real.  The only
difference is that this one streams: each block is shifted with its own
transform, and the phase is corrected by the block's absolute position so
the carrier stays continuous across block boundaries (a per-block reset would
put a phase jump into the middle of a burst, which a receiver sees as a
carrier phase jump).

Copyright (C) 2026 Rhizomatica
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import sys

import numpy as np

GUARD = 4096  # bench_common.c's SHIFT_GUARD


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


class OffsetChannel:
    def __init__(self, fs=8000, offset_hz=0.0, no_dbhz=None, block=4096, seed=1234):
        self.fs = fs
        self.offset_hz = offset_hz
        self.block = block
        self.nfft = next_pow2(2 * GUARD + block)
        self.dk = int(round(offset_hz * self.nfft / fs))
        self.n0 = 0            # absolute index of the next sample
        self.rng = np.random.default_rng(seed)
        self.no_dbhz = no_dbhz

    def _shift_block(self, x):
        """bench_freq_shift_real(), one block at a time."""
        n = len(x)
        a = np.zeros(self.nfft, dtype=np.complex128)
        a[GUARD:GUARD + n] = x
        A = np.fft.fft(a)
        # Analytic signal: double the positive half, zero the negative.
        A[1:self.nfft // 2] *= 2.0
        A[self.nfft // 2 + 1:] = 0.0
        # Circular shift by dk bins, dropping what would wrap past DC.
        w = np.zeros_like(A)
        k = np.arange(0, self.nfft // 2 + 1)
        src = k - self.dk
        valid = (src >= 0) & (src <= self.nfft // 2)
        w[k[valid]] = A[src[valid]]
        y = np.fft.ifft(w)
        # The transform's phase reference is the padded buffer's origin; move
        # it to the block's first sample, and then to the stream's origin, so
        # the carrier is continuous from block to block.
        corr = -2.0 * np.pi * self.dk * (GUARD + self.n0) / self.nfft
        y = y[GUARD:GUARD + n] * np.exp(1j * corr)
        self.n0 += n
        return y.real

    def process(self, x):
        y = self._shift_block(x) if self.offset_hz != 0.0 else x.copy()
        if self.no_dbhz is not None:
            # Noise density per Hz over the 0..fs/2 band: the same convention
            # as the model (SNR3k = -No - 14.82).
            var = 10.0 ** (self.no_dbhz / 10.0) * self.fs / 2.0
            y = y + self.rng.normal(0.0, np.sqrt(var), size=len(y))
        return y


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--freq", type=float, default=0.0, help="carrier offset in Hz")
    ap.add_argument("--no", dest="no_dbhz", type=float, default=None,
                    help="AWGN noise density in dB/Hz (default: clean)")
    ap.add_argument("--fs", type=int, default=8000)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()

    ch = OffsetChannel(fs=args.fs, offset_hz=args.freq, no_dbhz=args.no_dbhz, seed=args.seed)
    raw = sys.stdin.buffer
    out = sys.stdout.buffer
    carry = np.zeros(0, dtype=np.float64)
    moved = 0
    last_log = 0

    def emit(y):
        # The FIFO backend's format: s32le, the sample in the high 16 bits.
        v = np.clip(np.round(y), -32768.0, 32767.0).astype("<i4") << 16
        out.write(v.tobytes())

    while True:
        # read1(), not read(): a blocking read of 64 kB would wait for two
        # seconds of audio before handing anything to the modem, which is
        # longer than a CALL's retry interval -- the modem would hear the
        # preamble late, or not at all.
        data = raw.read1(65536)
        if not data:
            break
        arr = (np.frombuffer(data, dtype="<i4") >> 16).astype(np.float64)
        buf = np.concatenate([carry, arr])
        n = (len(buf) // ch.block) * ch.block
        for i in range(0, n, ch.block):
            emit(ch.process(buf[i:i + ch.block]))
        moved += n
        carry = buf[n:]
        if moved - last_log >= args.fs:
            last_log = moved
            print(f"channel: {moved} samples moved ({moved / args.fs:.1f} s)",
                  file=sys.stderr, flush=True)
    if len(carry):
        emit(ch.process(carry))
        moved += len(carry)
    out.flush()
    print(f"channel: offset {args.freq:+.1f} Hz, {moved} samples moved "
          f"({moved / args.fs:.1f} s), applied offset "
          f"{ch.dk * args.fs / ch.nfft:+.3f} Hz", file=sys.stderr)


if __name__ == "__main__":
    main()
