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

4. **AWGN**: Same scaling convention as `modem/freedv/ch.c` (No in dB/Hz, variance = Fs × 10^(No/10) × 10^6).

### API

```c
watterson_t w;
watterson_init(&w, 8000);
watterson_add_path(&w, delay_ms, doppler_hz, freq_hz, gain);
watterson_set_noise(&w, nodb);
watterson_process(&w, samples, n);
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
