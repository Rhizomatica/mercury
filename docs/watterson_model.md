# Watterson Model Implementation Summary

## Files Created/Modified

| File | Status | Purpose |
|------|--------|---------|
| `common/watterson.h` | **new** | API header — `watterson_t`, `watterson_path_t`, `watterson_init()`, `watterson_dispose()`, `watterson_add_path()`, `watterson_set_noise()`, `watterson_process()` |
| `common/watterson.c` | **new** | Implementation — Box-Muller Gaussian noise generator, 2nd-order Butterworth IIR Doppler-shaping filters, tapped-delay-line multi-path combining, optional AWGN |
| `utils/watterson_test.c` | **new** | CLI test utility (pipeline: real int16 → gain → Hilbert transform → clipper → freq shift → Watterson → AWGN → SSB filter → real int16 output) |
| `common/Makefile` | **modified** | Added `watterson.o` to `all:` and build rule with `-I..` |
| `utils/Makefile` | **modified** | Added `-I.. -I../modem/freedv` to CFLAGS, added `watterson_test` target |

## Design

Implements **ITU-R F.1487 Watterson narrowband HF ionospheric channel model**:

```
h(τ,t) = Σ g_i(t) · δ(τ - τ_i)
```

Each tap gain g_i(t) is a complex Gaussian random process whose power spectrum is:

```
S_i(f) = (1/(√(2π)·σ_i)) · exp(-f²/(2σ_i²))
```

### Key components

1. **Doppler shaping**: Box-Muller Gaussian noise → 2nd-order Butterworth IIR low-pass filter (fc = σ, bilinear transform). Each path has independent I/Q filters.

2. **Tapped delay line**: Circular buffer per path, configurable delay (ms).

3. **Frequency offset**: Complex rotation per path with phase accumulator.

4. **AWGN**: same scaling convention as `modem/freedv/ch.c` (No in dB/Hz, variance = Fs × 10^(No/10) × 10^6). The model accumulates the pre-noise faded-signal power and the actual added-noise power, so the true post-channel SNR is reported by `watterson_measured_snr3k()` (independent of the configured `noise_var`).

## Correctness fixes (post-landing)

Two bugs were fixed after the initial commit:

1. **`--No` ignored when placed after a preset.** The `--good`/`--moderate`/… (and `--path`) cases called `watterson_set_noise()` inline with whatever `No` had been parsed so far; with `getopt` permutation this silently used the default −100. **Fix:** `watterson_set_noise()` is now called **once, after option parsing**, so `--No` works in any position.

2. **Reported SNR didn't track `--No`.** The CLI computed SNR from `tx_pwr_fade`, which was summed **after** `watterson_process()` had already added the AWGN — i.e. |signal + noise|² — pinning the reported SNR near a constant as noise grew. **Fix:** the model now measures the pre-noise signal power and the actual added-noise power (`watterson_reset_meas()` / `watterson_measured_snr3k()`), and the CLI reports SNR from those.

After the fix the reported `SNR3k` tracks `--No` with the correct slope and **agrees with the modem's own measured SNR to ~2 dB**.

### SNR calibration vs `ch.c`

`watterson_test` reads ~3 dB **lower** than `modem/freedv/ch.c` at the same `--No` in AWGN-only mode (a signal-power convention difference; the modem's measured SNR sits between the two). When using SNR thresholds derived with `ch` (e.g. `docs/MODES.md`), read the **reported** `SNR3k` rather than assuming `ch`-equivalent `--No` values. Note the `--No` operating range also differs from `ch` (watterson needs a more-negative `--No` for the same SNR), and a fading preset further lowers the mean SNR by the (physical) fade loss.

### API

```c
watterson_t w;
watterson_init(&w, 8000);                /* also resets the SNR accumulators */
watterson_add_path(&w, delay_ms, doppler_hz, freq_hz, gain);
watterson_set_noise(&w, nodb);
watterson_process(&w, samples, n);       /* accumulates sig/noise power */
float snr3k = watterson_measured_snr3k(&w);   /* true post-channel SNR (dB) */
watterson_dispose(&w);
```

Supports up to 4 paths (`WATTERSON_MAX_PATHS`).

### Presets (CLI)

| Preset | Doppler σ | Delay | Paths |
|--------|-----------|-------|-------|
| `--good` | 0.1 Hz | 0.5 ms | 1 |
| `--moderate` | 1.0 Hz | 1.0 ms | 2 |
| `--poor` | 1.0 Hz | 2.0 ms | 2 |
| `--flutter` | 10.0 Hz | 2.0 ms | 2 |

### Custom paths

```
--path delay_ms,doppler_hz,freq_hz,gain
```

Repeatable for multi-path.

## Test Results

- All 109 existing unit tests pass (0 failures) — no regressions.
- `watterson_test` utility compiles clean (0 warnings) and produces valid int16 output.
- Verified: output size correct, all values in int16 range, fading modifies signal (only ~0.6% identical samples vs input with `--poor` preset).

## Usage Examples

```sh
# Build
cd common && make watterson.o
cd ../utils && make watterson_test

# AWGN only
./utils/watterson_test in.raw out.raw --No -95

# CCIR Poor channel
./utils/watterson_test in.raw out.raw --poor --No -95

# Custom 2 paths with ±10 Hz Doppler offset
./utils/watterson_test in.raw out.raw --path 0,1.0,10.0,0.5 --path 2,1.0,-10.0,0.5 --No -95
```

## Dependencies

- `modem/freedv/comp.h` — `COMP` type
- `modem/freedv/comp_prim.h` — complex math (`cmult`, `cadd`, `cconj`, `fcmult`, `cabsolute`)
- Standard C99 + `-lm`

No external libraries beyond what the project already uses.

## Sample rates above 8 kHz — the Doppler filter must run in double

The fading tap is a 2nd-order Butterworth driven by white noise (see *Design*).
As the cutoff ratio `fc/fs` shrinks its poles crowd the unit circle: at 48 kHz
with 1 Hz Doppler the ratio is 2e-5 and the poles sit within ~1e-4 of the
circle, where `float`'s ~6e-8 of relative resolution is enough to place one
**outside** it. The filter then diverges instead of fading, and the "measured
signal power" is the blow-up rather than the signal:

    fs= 8000  2 paths  1 Hz  ->  SNR3k  -8.37 dB   sane
    fs=48000  1 path   0 Hz  ->  SNR3k  -8.24 dB   sane (no Doppler filter)
    fs=48000  2 paths  1 Hz  ->  SNR3k +65.76 dB   diverged

Coefficients and filter state are therefore `double` (`watterson.h`, the
`x_i/y_i/x_q/y_q` history and `b0..a2`). After the change the same case reads
−6.43 dB and 8 kHz is unmoved (−8.37 → −8.32, inside the scatter of a 2 s
window of slow fading). Mercury runs at 8 kHz, which is why this sat unnoticed:
8 kHz is 6× further from the cliff and stayed marginally stable. It surfaced
only when driving a 48 kHz modem through the same channel, and it matters for
any move to wider bandwidth.

`tests/common/test_watterson.c` pins two properties and was verified to fail on
the old code:

- fading stays finite and in range over 8/16/24/48/96 kHz × 0.1..2 Hz. The band
  is deliberately wide — it is there to catch divergence, not to pin a value.
- **a single static path is rate-invariant** to 0.5 dB (measured: 0.04 dB).
  That is the property that lets an 8 kHz result be compared with a 48 kHz one.

### Known wart: two-path measurements are not rate-invariant

A *single* static path agrees across rates to 0.04 dB, but adding a delayed
second path introduces a multipath interference term that genuinely varies with
sample rate — measured **+1.56 dB at 48 kHz vs 8 kHz** for an identical static
two-path channel (2 ms delay, 1500 Hz tone, 8 s window). The test therefore
asserts rate-invariance only for the single-path case.

Consequence for cross-rate comparisons: absolute *faded* SNR3k figures measured
at different sample rates carry a ~1.5 dB systematic offset and should be
corrected or compared like-for-like. Single-path/AWGN comparisons are unaffected.
