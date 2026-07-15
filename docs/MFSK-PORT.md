# MFSK (v1 cl_mfsk) ported to C — and MFSK-vs-FSK comparison

Follow-on to docs/LOW-SNR-FLOOR-PLAN.md (which found a non-coherent FSK
weak-signal mode is the real lever for the deep fringe, and codec2's 2-FSK
`FSK_LDPC` already out-performs DATAC15 there). This ports Mercury **v1's
32-MFSK** (which reached ~−13 dB Es/N0) from C++ to pure C, and measures how
much its higher modulation order M buys over 2/4-FSK.

## The port — `modem/mfsk.{c,h}`
Faithful pure-C port of v1's `cl_mfsk` (originally Fadi Jerji, C++):
non-coherent M-FSK over OFDM subcarriers. `class` → `struct mfsk_t` + functions;
`std::complex<double>` → a plain `{double re, im}` struct (zero-ambiguity FFT
interop); `std::isfinite` → C99 `isfinite`. All tone tables (preamble,
Welch-Costas ACK/BREAK, Sidelnikov NB, directed-HAIL FNV-1a suffix) carried over
verbatim. Compiles clean under `-std=gnu11 -Wall -Wextra`.

Validated by `tests/modem/test_mfsk.c` (in the unit suite):
- **mod→demod round-trip is lossless** with no noise for M=4/8/16/32
  (Gray-code + tone-hop consistency).
- soft-LLR sign tracks the sent bit; higher M is more robust at fixed Eb/N0.

## MFSK-vs-FSK: measured modulation-order gain
Non-coherent, uncoded **BER vs Eb/N0**, driving the ported C demod through
frequency-domain AWGN / interleaved Rayleigh (the OFDM subcarrier-SNR model the
demod assumes). Nc=50, 1 stream, 20 k symbols.

**AWGN:**

| Eb/N0 | M=2 | M=4 | M=8 | M=16 | M=32 |
|---|---|---|---|---|---|
| 8 dB | 0.021 | 0.0020 | 0.0002 | 0.0000 | 0.0000 |
| 6 dB | 0.068 | 0.016 | 0.0039 | 0.0013 | 0.0002 |
| 4 dB | 0.142 | 0.062 | 0.031 | 0.016 | 0.0089 |

**Interleaved Rayleigh (per-symbol, non-coherent):**

| Eb/N0 | M=2 | M=4 | M=8 | M=16 | M=32 |
|---|---|---|---|---|---|
| 16 dB | 0.023 | 0.014 | 0.014 | 0.011 | 0.010 |
| 12 dB | 0.054 | 0.034 | 0.032 | 0.027 | 0.025 |
| 8 dB | 0.118 | 0.080 | 0.070 | 0.063 | 0.060 |

**Reading it** (at ~1 % BER, a typical LDPC-input operating point):
- **AWGN:** M=32 needs ~4 dB vs M=2's ~10 dB → **~6 dB power-efficiency gain.**
- **Rayleigh:** the gain compresses to **~1.5–2 dB** — under fast fading the fade
  (not the modulation order) dominates, and non-coherent detection of a faded
  symbol suffers regardless of M.

## Conclusion
1. The big fringe win is **going non-coherent FSK at all** (already shown: even
   2-FSK beats coherent DATAC15 on MPP).
2. v1's **32-MFSK adds a real ~6 dB on AWGN / steady (calm-NVIS) channels**, but
   only ~1.5–2 dB under fast fading. Worth it where the channel is steady; less
   so in deep fast fading.
3. The C port makes v1's 32-MFSK available to slot into a real v2 weak-signal
   mode (ARQ ladder bottom rung) — the OFDM framing (bins↔time FFT), preamble
   sync, and LDPC glue remain to wire it up as an actual mode (follow-up).

## Reproduce
- Round-trip / gain unit test: `cd tests && make test_mfsk && ./test_mfsk`.
- BER sweeps: the throwaway harness `mfsk_ber.c` (scratch) links `modem/mfsk.c`,
  adds freq-domain AWGN/Rayleigh at a target Eb/N0, and counts hard-decision bit
  errors; swept over M∈{2,4,8,16,32}.
