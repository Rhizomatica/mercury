# Watterson channel model: the standard's Doppler spectrum and named presets

**Status:** design record for the `channel-model` change, 2026-10-10; revised 2026-10-12 after review
**Author:** Joseph Freivald
**Scope:** `common/watterson.{c,h}`, `utils/watterson_test`, `utils/chanutil` and the three utilities that use it, `tests/common/test_watterson.c`, `tests/integration` (bench preflight and tool tests), this record and its figures. No modem change, no wire change.

## Why

Every fading number this project measured with the Watterson engine
(`MERCURY_CH_ENGINE=watterson`, `utils/watterson_test`, `utils/chanutil`) goes
through `common/watterson.c`. Not every fading number in the documentation
does: `docs/MODES.md` was measured on codec2's `ch`, and section 4 of
`docs/FADE-CLIFF-DECISION.md` on pathsim. See "Which document was measured on
which engine" below.

Up to and including release 1.9.19, the taps of `watterson.c` were shaped by a
second-order Butterworth low-pass filter. It ran at the sample rate, with its
3 dB cutoff at the path's `doppler_hz`. The one-off comparison recorded below
ran a tone through the channel and took the spectrum of the complex envelope.
Measured that way, the old process is not the one the standards define. Every
measured number in this section is a cell of
`docs/channel-figures/validation.csv`; the Validation section explains the
columns. Some reference values are analytic, not measured. For a Gaussian
spectrum, 4.55 % of the power lies beyond 2 sigma and 0.27 % beyond 3 sigma.
For a second-order Butterworth, 21.95 % lies beyond its cutoff and 1.11 %
beyond 3 root-mean-square (rms) widths. The Rice closed form for the fade
rate is given in the Validation section.

- **Wrong width.** ITU-R F.1487 states a path's "Doppler spread" as twice
  the standard deviation (2 sigma) of a Gaussian spectrum. CCIR 520-2 is
  reported to use the same definition; no copy of it was available. The
  Butterworth cutoff is the spectrum's rms width. So the
  legacy preset `poor`, labelled "1 Hz", has an rms width of 0.9994 Hz, which
  is a 2.00 Hz spread, where CCIR Poor has 0.5 Hz. A user who typed 1 Hz got a
  process with 21.96 % of its power beyond 1 Hz. The standard's 1 Hz Gaussian
  has 4.55 % beyond 1 Hz. `ccir-poor` measures 4.45 %. The legacy channel
  fades at about twice the rate: 1.0067 fades below −10 dB per second (`poor`,
  seed 23, 900 s) against 0.5151 for `ccir-poor` (seed 9, 900 s). The Rice
  closed form gives 1.0137 for the legacy width and 0.5072 for the standard's.
  MATLAB's `comm.RayleighChannel` gives 0.4889 on the same seed and record as
  `ccir-poor`.
- **Wrong shape.** A second-order Butterworth falls at 12 dB per octave; a
  Gaussian falls faster and faster. Compare the spectra at the same rms width.
  The seven legacy rows have 1.13 to 1.21 % of their power beyond three times
  the rms width. The analytic value for a second-order Butterworth is 1.11 %.
  The standard rows have 0.20 to 0.35 % (Gaussian: 0.27 %). Against a Gaussian of the
  measured width, the legacy spectra differ by 1.72 to 1.98 dB rms over
  ±3 sigma. The standard rows differ by 0.06 to 0.65 dB, which is the noise
  floor of the estimate (see "Statistical resolution"). The shape is a
  second-order effect on the fading. The fade rate below −10 dB depends on the
  rms width alone: the Rice closed form predicts every legacy row from its
  measured width within 3.5 %. The width error, not the shape, is what made
  the legacy channel twice as fast.
- **Wrong presets.** `good` had one path and no multipath at all. `moderate`
  used a 1 Hz cutoff where CCIR Moderate is a 0.5 Hz spread. `flutter` used
  2 ms where CCIR Flutter is 0.5 ms.
- **Numerically fragile.** At 48 kHz with a cutoff of 1 Hz or less, the Butterworth poles lie within 1e-4 of
  the unit circle. `common/watterson.h` records the measurement that forced the
  filter into double precision: a diverging filter was reported as an SNR3k of
  +65.76 dB. SNR3k is the signal-to-noise ratio (SNR) referenced to a 3 kHz
  bandwidth. `tests/common/test_watterson.c` guards the filter.
- **One sample short.** Found by the new validation. The delay line wrote the
  current sample and then read the next slot. So every path arrived one sample
  earlier than configured. At 8 kHz a 1 ms path arrived after 7 samples, a
  2 ms path after 15, and a 0.5 ms path after 3. A path shorter than one
  sample had no delay at all.

The consequence: a Mercury result on `poor` could not be compared with a VARA
or ARDOP result on CCIR Poor. A fading margin measured on `poor` was a margin
against a channel about twice as fast as the one named.

![Doppler spectrum, legacy versus standard](channel-figures/doppler_spectrum.svg)

![Fade rate at the CCIR points](channel-figures/fade_rate.svg)

## What changes

**The tap process.** Each fading path is a complex Gaussian process with a
Gaussian Doppler power spectrum. Its spread (2 sigma) is the path's
`doppler_hz`. The generator works like this. White complex Gaussian noise is
drawn at a low rate fs / R, where fs is the sample rate and
R = floor(fs / (64 × spread)). The low rate is between 64 and 128 times the
spread. A finite impulse response (FIR) filter shapes that noise. Its taps are
the sampled impulse response exp(−4 π² σ² t²), with σ = spread / 2. The taps
are cut where the response has fallen to 1e-4 and scaled so that
E[|tap|²] = 1. The result is linearly interpolated to the sample rate. The
filter depends only on the ratio of the spread to the low rate, so the same
spread gives the same process at any sample rate. The FIR history is
pre-filled with noise, so a tap starts in its steady-state distribution and
needs no warm-up. `doppler_hz = 0` gives a static path.
`watterson_add_path()` returns −1 for a non-finite argument, a negative delay
or spread, or a gain outside [0, 1]. It also returns −1 for a spread below
about fs / 2^36 (1.2e-7 Hz at 8 kHz). Below that value the generator's
integer sizes overflow.

**The delay line** reads its slot before writing it, so a path is delayed by
exactly ceil(delay_ms × fs / 1000) samples. A custom delay is rounded up to a
whole sample: 0.3 ms at 8 kHz becomes 3 samples, which is 0.375 ms. Every
preset delay is a whole sample at 8, 12, 16 and 48 kHz (not at 9600 or
44100 Hz). The legacy spectrum keeps the old line, one sample short, so that
its pinned realisations are unchanged. `tests/common/test_watterson.c` pins
both behaviours. The tool's path report prints the delay as configured and as
realised. `ccir-poor` prints `delay=2.000 ms (16 samples = 2.0000 ms)`. The
legacy `poor` prints `(15 samples = 1.8750 ms)`.

**The parameter.** `doppler_hz` is the standard's Doppler spread, 2 sigma. The
Butterworth taps are kept as `WATTERSON_SPECTRUM_LEGACY`, selected with
`watterson_set_spectrum()` before `watterson_add_path()`, or with
`watterson_test --legacy`. There `doppler_hz` keeps its old meaning, the
filter cutoff. The default of `watterson_init()` is the Gaussian spectrum. So
a `--path` on the command line, and any `watterson_add_path()` caller that does
not select LEGACY, now gets the standard taps and the exact delay line. The
seven named legacy presets are not affected: they select LEGACY themselves.

**Named presets.** One table in `watterson.c`. `watterson_test` takes `--NAME`
or `--preset NAME` and prints the table with `--list-presets`. The integration
bench passes `MERCURY_CH_FADING=NAME` through as `--NAME`. `chanutil` accepts
the same names, typed in upper-case or lower-case letters.

| preset | source | delay | Doppler spread | same parameters as |
|---|---|---|---|---|
| `ccir-good` | CCIR 520-2 Good | 0.5 ms | 0.1 Hz | `itur-mq` |
| `ccir-moderate` | CCIR 520-2 Moderate | 1 ms | 0.5 Hz | `itur-mm`, `itur-hq` |
| `ccir-poor` | CCIR 520-2 Poor | 2 ms | 1 Hz | `itur-md` |
| `ccir-flutter` | CCIR 520-2 Flutter | 0.5 ms | 10 Hz | none; F.1487 has no flutter case |
| `itur-lq` / `itur-lm` / `itur-ld` | F.1487 low latitude quiet / moderate / disturbed | 0.5 / 2 / 6 ms | 0.5 / 1.5 / 10 Hz | |
| `itur-mq` / `itur-mm` / `itur-md` | F.1487 mid latitude quiet / moderate / disturbed | 0.5 / 1 / 2 ms | 0.1 / 0.5 / 1 Hz | `ccir-good` / `ccir-moderate` / `ccir-poor` |
| `itur-mdv` | F.1487 mid latitude disturbed, NVIS (near-vertical-incidence skywave) | 7 ms | 1 Hz | |
| `itur-hq` / `itur-hm` / `itur-hd` | F.1487 high latitude quiet / moderate / disturbed | 1 / 3 / 7 ms | 0.5 / 10 / 30 Hz | `itur-hq` = `itur-mm` = `ccir-moderate` |

Every standard preset is two paths of equal power (gain 0.7071 each) with no
frequency shift. Presets with the same parameters build the same channel on
the same seed; the validation gives every row its own seed for that reason.

Sources. The ten `itur-*` rows are the values of ITU-R F.1487 Annex 3. In the
one-off comparison of 2026-10-12 (below) they were also checked against
MATLAB's `stdchan('iturHF**', 8000, 1)` (Communications Toolbox). The delay is
`PathDelays(2)`. The spread is 2 × `NormalizedStandardDeviation` with
`MaximumDopplerShift` = 1 Hz. The 2 sigma definition of the spread is taken
from F.1487 Annex 3. The review of this change reports that F.1487 Annex 3
names a source for its mid-latitude quiet, moderate and disturbed values.
That source is the good, moderate and poor channels of the former
Recommendation ITU-R F.520. The author did not check this against a copy of
F.1487. It is the source of `ccir-good`, `ccir-moderate` and `ccir-poor`.
F.1487 has no flutter case. The
`ccir-flutter` values (0.5 ms, 10 Hz) are the CCIR 520-2 flutter channel. No
copy of CCIR 520-2 was available, so no clause of it is cited.

**Legacy presets.** The names `good`, `moderate`, `poor`, `flutter`, `mpg`,
`mpp`, `mpd` keep their Butterworth taps, their one-sample-short delay line
and their numbers. "Cutoff" is the Butterworth 3 dB frequency, which is also
the rms width; "measured spread" is twice the rms width measured in
`validation.csv`.

| name | paths | delay | cutoff | gain per path | measured spread |
|---|---|---|---|---|---|
| `good` | 1 | none | 0.1 Hz | 0.8 | 0.20 Hz |
| `moderate` | 2 | 1 ms | 1.0 Hz | 0.7 | 1.95 Hz |
| `poor` | 2 | 2 ms | 1.0 Hz | 0.7 | 2.00 Hz |
| `flutter` | 2 | 2 ms | 10 Hz | 0.7 | 19.70 Hz |
| `mpg` | 2 | 0.5 ms | 0.1 Hz | 0.7 | 0.20 Hz |
| `mpp` | 2 | 1 ms | 1.0 Hz | 0.7 | 1.97 Hz |
| `mpd` | 2 | 2 ms | 2.0 Hz | 0.7 | 3.92 Hz |

The legacy `mpg`, `mpp`, `mpd` are not the MPG, MPP, MPD of codec2's `ch`
(the channels of `docs/MODES.md`). The `ch` MPP is 2 ms with a Gaussian
spectrum. The Watterson `mpp` is 1 ms with a 1 Hz Butterworth cutoff, about a
2 Hz spread. The bench passes the same name to either engine, so the engine
decides which channel runs. Before this change `watterson_test` did not know
`--mpg`, `--mpp` and `--mpd`: it printed "unrecognized option" and exited 0
with an empty output file, while `chanutil` accepted them. Now both accept
them, as the one legacy channel each name has always meant in `chanutil`. New
measurements should use the standard names.

**The tool** (`utils/watterson_test`). Options may be placed before or after
the two file names. Every option name must be typed in full. The 1.9.18 tool
accepted any unique abbreviation (`--po` ran `--poor`); an earlier draft of
this change ran `itur-mq` for `--itur-m`. Both are rejected now. An unknown
option now exits 1 with a short message. The 1.9.18 tool printed the usage
text, exited 0 and left an empty output file. `--list-presets` prints the
table to stdout and exits 0. `--help` exits 0. The channel is built once,
after all options are read, so `--Fs` works in any position. Before this
change, `--Fs` after a preset left the channel at 8000 Hz while the banner
printed the new rate. `--legacy` applies to every `--path` of the run, is not
allowed with a standard preset, and a run that mixes two spectra is refused.
The path report names the spectrum of each path (`spectrum=gaussian` or
`spectrum=legacy`) and the realised delay. The printed SNR3k uses the `ch.c`
convention: the faded analytic-signal power over the complex noise power in
3 kHz, measured before the SSB (single sideband) filter. It equals the in-band
SNR of the output with `--ssbfilt 1` (the default) or with `--complexout`.
With `--ssbfilt 0` and real output it reads 3.01 dB high. The printed number
is unchanged, and `SNR3k = −No − 14.82` still holds.

**`chanutil`** and the utilities that use it (`hail_suffix_sweep`,
`acquire_vs_decode`, `burst_train_probe`): preset names are case-insensitive;
`MPP`, `MPG`, `MPD` are labelled as the legacy Butterworth presets; the usage
texts name the legacy presets and point to `watterson_test --list-presets` for the standard names. The comment on the warm-up in `chanutil_open()` is now
correct. The legacy IIR (infinite impulse response) filter needs the warm-up.
On a Gaussian preset it only flushes the delay line (at most 56 samples).

**The integration bench** (`tests/integration/channel_bridge_test.go`). A
watterson preset name is checked before the bench starts. The tool is run once
on an empty input with the exact arguments the bench will pass. If it exits
non-zero, the test fails with the tool's message. The channel process's
stderr (seed, path report, summary) is kept and printed. A channel process
that exits non-zero now stops that direction with a message. Before this
change it was respawned with no message. When the name is `mpg`, `mpp` or
`mpd`, the bench logs a warning that this is the tool's legacy Butterworth
channel, not `ch`'s profile. `TestWattersonLegacyNote` checks that warning.
`tests/integration/watterson_tool_test.go` (new) covers the tool's command
line.

**Noise.** The model's noise is AWGN (additive white Gaussian noise) only:
`--No` in dB/Hz, SNR referenced to 3 kHz. There is no impulsive noise.

**Seeds.** `MERCURY_WATTERSON_SEED` pins the realisation, and the tool echoes
it as "(pinned)". The tap draws and the noise draws come from one random
stream. So a seed names one fade only for one configuration: the same
spectrum, paths, sample rate and AWGN setting. Change `--No` from on to off,
or the sample rate, and the same seed is a different fade.

**Scope of the model.** The presets use the single-component form of F.1487
that `stdchan` also uses: one Gaussian component per path, equal spreads on
both paths, no frequency shift. A two-component (bi-Gaussian) tap, which
F.1487 allows, is built from two paths at the same delay with different
`freq_hz`, each with its own spread; no preset uses it. A per-path frequency
shift is a constant offset (`freq_hz` in `--path` or in the C API, the
application programming interface), not a drift. The tool's `--freq` rotates
the whole signal before the channel. Gain control (AGC, automatic gain
control; ALC, automatic level control), transmitter and receiver filter shapes
and sample-rate offset are outside the model.

## Validation

### Gates run by CI

`tests/common/test_watterson.c` is the validation that CI runs on every
change: in Debian 13 containers on amd64 and i386, and the same binary under
AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer. It has 17
tests. Two of them are the gates that separate the standard's spectrum from
the old one. Both run on a unit DC input through one path (the output is the
tap itself) for 900 s at 8 kHz, decimated to 64 Hz, on pinned seeds. The
values below are the ones the test prints with `WATTERSON_TEST_VERBOSE=1`.

| test | what it measures | reference | gate | measured, pinned seed |
|---|---|---|---|---|
| spectrum tails | the tap's power beyond 3 sigma and beyond the spread, from a Welch spectrum (Hann window, 16 s segments, the test's own FFT with a self-check) | Gaussian: 0.27 % beyond 3 sigma, 4.55 % beyond the spread. Second-order Butterworth: 1.11 % beyond 3 × its cutoff, 21.95 % beyond the cutoff | Gaussian < 0.6 % and 4.55 ± 2.0 points; legacy > 0.9 % and > 15 % | Gaussian 1 Hz, seed 13: 0.273 % and 4.68 %, rms width 0.5010 Hz. Legacy 1 Hz, seed 113: 1.081 % and 21.66 %, rms width 0.9775 Hz |
| fade rate | downward crossings of −10 dB below the record's rms, per second, on the same 64 Hz record | Rice closed form 2 sqrt(pi) sigma rho exp(−rho²), rho = 10^(−10/20): 0.507 × spread. For the legacy path, the Rice rate for its own measured rms width | 3 × 0.74 / sqrt(N), N = the expected count: 10.4 % at N = 456, 7.4 % at N = 891. The legacy path must also fade at least 1.7 times as fast as the Gaussian path of the same number | Gaussian 1 Hz, seed 17: 0.5067 /s, 456 fades, −0.1 % from 0.5072. Legacy 1 Hz, seed 117: 0.9444 /s, 850 fades, −4.6 % from 0.9900. Ratio 1.86 |

The test's own comment records why the legacy path is held to its measured
width rather than to its cutoff (the band-limited record's width is 2 %
under the cutoff, and the Butterworth tail's quick double crossings are
missed at 64 Hz), and the count's scatter measured over 100 seeds
(0.71 sqrt(N) for the Gaussian path, 0.79 sqrt(N) for the legacy path), so
each gate is about 3 sigma. The other 15 tests are listed under "C and Go
tests" below.

### One-off comparison with MATLAB (dated record)

The tables that follow were measured once, on 2026-10-12, with a MATLAB
script (R2026a, Signal Processing Toolbox and Communications Toolbox). The
script is not part of the repository: Mercury keeps one set of tests, the C
unit tests and the Go bench, and CI runs them. The script's results are kept
here as data (`docs/channel-figures/validation.csv`, `fade_rate.csv` and the
two figures). The script ran every preset through `utils/watterson_test` and
measured the tap process:

1. A tone at 1/8 of the sample rate (1 kHz at 8 kHz, 6 kHz at 48 kHz) is
   written as int16. It is run through `watterson_test --<preset> --Fs <fs>
   --No -100 --ssbfilt 0` with `MERCURY_WATTERSON_SEED` set. The output's
   complex envelope is the sum of the fading taps.
2. The tool's path report is parsed and compared with the standard: two
   paths, `spectrum=gaussian`, the reported Doppler, and the second path's
   delay in samples and in milliseconds.
3. The envelope is demodulated, lowpass filtered at 200 Hz and decimated to
   64 times the spread. A 48 kHz record is decimated to 8 kHz first. The
   first 5 s are dropped. Every statistic is computed on this record.

Pass conditions the script applied to the standard presets. Every condition
must hold for the row to pass.

| quantity | reference | gate | why this gate |
|---|---|---|---|
| rms width of the Doppler spectrum | spread / 2 | 6 % | one record resolves the width to about 1.5 % (1 sigma) at spreads up to 1.5 Hz. The 11 such rows in `validation.csv` scatter with an rms of 1.48 %. 6 % is 4 sigma. The errors this change fixes are 100 %. |
| power beyond the spread | Gaussian: 4.55 % | ±2 percentage points | a Butterworth with its cutoff at the spread has 21.95 % |
| power beyond 3 × the measured rms width | Gaussian: 0.27 % | < 0.6 % | shape only, independent of the width; any second-order Butterworth has 1.11 % |
| spectrum shape: rms error in dB over abs(f) ≤ 3 sigma against a Gaussian of the measured width, both at unit power | 0 | < 1.2 dB | a true Gaussian measures 4.34 / sqrt(K) dB, where K is the number of segments that Welch's spectrum estimate averages. That is 0.59 dB at K = 54 or 55 (0.1 and 0.5 Hz) and 0.07 dB at K = 3495 (30 Hz). The legacy rows measure 1.72 to 1.98 dB. |
| amplitude distribution | Rayleigh | Kolmogorov distance < 0.03 | the largest difference between the measured and the Rayleigh cumulative distribution; the standard rows measure 0.0008 to 0.024 |
| fixed component | Rice K factor, the power of the mean over the mean power | < −20 dB | a Rayleigh test alone is weak against a small fixed component; this measures it directly. The standard rows measure −24 to −60 dB. |
| fades below −10 dB per second | Rice closed form N = 2 sqrt(pi) sigma rho exp(−rho²), rho = 10^(−10/20), which is 0.507 × spread | 3 × 0.74 / sqrt(N), N = the expected count | the count of one record scatters by about 0.74 sqrt(N) fades (1 sigma) |
| MATLAB `comm.RayleighChannel`, Gaussian Doppler, same spread, seed and record | logged, not gated | | its count has the same scatter, so the model-to-MATLAB ratio on one seed is uncertain by about 7 % (1 sigma) at the slow presets |
| reported delay and Doppler | `stdchan('iturHF**', 8000, 1)` for `itur-*`; the CCIR 520-2 table for `ccir-*` | exact | |
| delay-line echo: an impulse train through two static paths at the standard's delay | ceil(delay × fs / 1000) samples | exact | |

Legacy presets were measured with the same script. Two conditions apply,
because they are true of the legacy model. One is Rayleigh amplitude (Kolmogorov distance
< 0.03). The other is the fade rate against the Rice rate for the measured
rms width, within the same count tolerance. Their delay line is checked to be
one sample short of
ceil(delay × fs / 1000), and the path report to say `spectrum=legacy`. The
script also checked that `--list-presets` printed the same names in the same
order as its own list, because its seeds depend on that order. It made 26
checks: that list check, 14 standard presets at 8 kHz, `ccir-moderate` and
`itur-hd` at 8 and 48 kHz (4), and 7 legacy presets.

Seeds and records. Seed = 7 + the preset's index in `--list-presets` order:
`ccir-good` 7 … `itur-hd` 20, `good` 21 … `mpd` 27. The two 48 kHz rows add
100. MATLAB's reference channel uses the same seed as the row. Record length
= max(900 s, 450 / spread): 4500 s at 0.1 Hz, 900 s at 0.5 Hz and above. That
is about 450 cycles of the spread at 0.1 and 0.5 Hz, and 900 to 27 000 cycles
at the others.

### Statistical resolution

**Doppler width.** One record of about 450 cycles resolves the rms width to
about 1.5 % (1 sigma). The 11 standard rows at 0.1 to 1.5 Hz have width
errors between −2.22 % and +3.36 %, rms 1.48 %. The five rows at 10 and
30 Hz (9000 to 27 000 cycles) are within 0.33 %. A statement finer than
about 2 % at the slow presets needs more seeds or longer records.

**Fade rate.** The expected count N of fades below −10 dB in one record comes
from the Rice closed form. The count scatters by about 0.74 sqrt(N)
(1 sigma). The gate is three times that (the `fade_tol_pct` column).

| spread | record | expected fades N | 1 sigma of the count | gate (3 sigma) |
|---|---|---|---|---|
| 0.1 Hz | 4500 s | 228 | 4.9 % | 14.7 % |
| 0.5 Hz | 900 s | 227 | 4.9 % | 14.7 % |
| 1 Hz | 900 s | 454 | 3.5 % | 10.4 % |
| 1.5 Hz | 900 s | 681 | 2.8 % | 8.5 % |
| 10 Hz | 900 s | 4539 | 1.1 % | 3.3 % |
| 30 Hz | 900 s | 13 617 | 0.63 % | 1.9 % |

The 16 standard rows are within 7.07 % of the Rice rate (worst: `itur-lq`,
243 fades counted against 227 expected). MATLAB's realisations are within
8.67 % of Rice. The model-to-MATLAB ratio ranges from −8.1 % to +6.2 %, which
is what two counts of about 230 give. It is not a bias measurement.

**Shape.** The dB-error estimate has a noise floor of 4.34 / sqrt(K) dB,
where K is the number of Welch segments. The standard rows measure 0.06 to
0.65 dB. The column `shape_noise_db` gives the floor of each row.

| spread | K (Welch segments) | noise floor |
|---|---|---|
| 0.1 and 0.5 Hz | 54 or 55 | 0.59 dB |
| 1 Hz | 110 | 0.41 dB |
| 1.5 Hz | 167 | 0.34 dB |
| 10 Hz | 1074 | 0.13 dB |
| 30 Hz | 3495 | 0.07 dB |

**Independent realisations.** The 16 standard rows are 16 seeds, so 16
independent realisations. They cover 10 distinct parameter sets and 6
distinct spreads (0.1, 0.5, 1, 1.5, 10, 30 Hz). `ccir-good` and `itur-mq`,
`ccir-moderate`, `itur-mm` and `itur-hq`, and `ccir-poor` and `itur-md` have
the same parameters and are measured on different seeds.

### Measured

From `docs/channel-figures/validation.csv`, written by the script. Run of
2026-10-12, MATLAB R2026a Update 4, Apple-silicon Mac: 26 of 26 checks passed,
in 65 s. A second run reproduced the CSV byte for byte. Every value below is a cell of that file, except three kinds of
computed value:

- The spreads in brackets in the legacy table are twice the measured rms
  width.
- The "one short of" values are ceil(delay × fs / 1000).
- The gate percentages are rounded to one decimal.

The Gaussian and Butterworth values in the column heads are analytic.

Standard presets, Doppler spectrum:

| preset | delay / spread | record, seed | rms width, Hz (standard) | error | beyond the spread (Gaussian 4.55 %) | beyond 3 sigma (Gaussian 0.27 %) | shape, dB rms (noise floor) |
|---|---|---|---|---|---|---|---|
| `ccir-good` | 0.5 ms / 0.1 Hz | 4500 s, 7 | 0.0498 (0.05) | -0.41 % | 4.23 % | 0.301 % | 0.59 (0.59) |
| `ccir-moderate` | 1 ms / 0.5 Hz | 900 s, 8 | 0.2584 (0.25) | +3.36 % | 4.75 % | 0.197 % | 0.65 (0.59) |
| `ccir-moderate` (48 kHz) | 1 ms / 0.5 Hz | 900 s, 108 | 0.2496 (0.25) | -0.14 % | 4.31 % | 0.299 % | 0.55 (0.59) |
| `ccir-poor` | 2 ms / 1 Hz | 900 s, 9 | 0.5059 (0.50) | +1.18 % | 4.45 % | 0.220 % | 0.41 (0.41) |
| `ccir-flutter` | 0.5 ms / 10 Hz | 900 s, 10 | 5.0167 (5.00) | +0.33 % | 4.54 % | 0.258 % | 0.15 (0.13) |
| `itur-lq` | 0.5 ms / 0.5 Hz | 900 s, 11 | 0.2495 (0.25) | -0.19 % | 4.36 % | 0.279 % | 0.57 (0.59) |
| `itur-lm` | 2 ms / 1.5 Hz | 900 s, 12 | 0.7394 (0.75) | -1.42 % | 4.38 % | 0.294 % | 0.34 (0.34) |
| `itur-ld` | 6 ms / 10 Hz | 900 s, 13 | 5.0116 (5.00) | +0.23 % | 4.61 % | 0.257 % | 0.12 (0.13) |
| `itur-mq` | 0.5 ms / 0.1 Hz | 4500 s, 14 | 0.0497 (0.05) | -0.69 % | 4.50 % | 0.292 % | 0.62 (0.59) |
| `itur-mm` | 1 ms / 0.5 Hz | 900 s, 15 | 0.2468 (0.25) | -1.29 % | 4.23 % | 0.310 % | 0.61 (0.59) |
| `itur-md` | 2 ms / 1 Hz | 900 s, 16 | 0.5059 (0.50) | +1.18 % | 4.71 % | 0.246 % | 0.35 (0.41) |
| `itur-mdv` | 7 ms / 1 Hz | 900 s, 17 | 0.5041 (0.50) | +0.82 % | 4.53 % | 0.258 % | 0.38 (0.41) |
| `itur-hq` | 1 ms / 0.5 Hz | 900 s, 18 | 0.2445 (0.25) | -2.22 % | 4.26 % | 0.349 % | 0.58 (0.59) |
| `itur-hm` | 3 ms / 10 Hz | 900 s, 19 | 5.0050 (5.00) | +0.10 % | 4.55 % | 0.286 % | 0.13 (0.13) |
| `itur-hd` | 7 ms / 30 Hz | 900 s, 20 | 15.0380 (15.00) | +0.25 % | 4.57 % | 0.264 % | 0.07 (0.07) |
| `itur-hd` (48 kHz) | 7 ms / 30 Hz | 900 s, 120 | 15.0087 (15.00) | +0.06 % | 4.55 % | 0.266 % | 0.06 (0.07) |

Standard presets, amplitude, fading and delay:

| preset | Kolmogorov distance (KS) to Rayleigh | K factor, dB | fades/s: model / Rice / MATLAB | fades counted (expected); gate | echo, samples: measured (expected) |
|---|---|---|---|---|---|
| `ccir-good` | 0.0091 | -32.7 | 0.0514 / 0.0507 / 0.0487 | 231 (228); 14.7 % | 4 (4) |
| `ccir-moderate` | 0.0054 | -26.6 | 0.2503 / 0.2536 / 0.2722 | 224 (227); 14.7 % | 8 (8) |
| `ccir-moderate` (48 kHz) | 0.0128 | -36.3 | 0.2547 / 0.2536 / 0.2756 | 228 (227); 14.7 % | 48 (48) |
| `ccir-poor` | 0.0056 | -43.1 | 0.5151 / 0.5072 / 0.4889 | 461 (453.9); 10.4 % | 16 (16) |
| `ccir-flutter` | 0.0023 | -37.6 | 5.0547 / 5.0716 / 5.0700 | 4524 (4539.1); 3.3 % | 4 (4) |
| `itur-lq` | 0.0109 | -37.7 | 0.2715 / 0.2536 / 0.2556 | 243 (227); 14.7 % | 4 (4) |
| `itur-lm` | 0.0057 | -34.8 | 0.7274 / 0.7607 / 0.7689 | 651 (680.9); 8.5 % | 16 (16) |
| `itur-ld` | 0.0021 | -60.1 | 5.0648 / 5.0716 / 5.0522 | 4533 (4539.1); 3.3 % | 48 (48) |
| `itur-mq` | 0.0146 | -30.2 | 0.0512 / 0.0507 / 0.0538 | 230 (228); 14.7 % | 4 (4) |
| `itur-mm` | 0.0095 | -24.3 | 0.2581 / 0.2536 / 0.2556 | 231 (227); 14.7 % | 8 (8) |
| `itur-md` | 0.0086 | -40.1 | 0.4927 / 0.5072 / 0.5322 | 441 (453.9); 10.4 % | 16 (16) |
| `itur-mdv` | 0.0076 | -45.6 | 0.5162 / 0.5072 / 0.5211 | 462 (453.9); 10.4 % | 56 (56) |
| `itur-hq` | 0.0240 | -26.2 | 0.2626 / 0.2536 / 0.2722 | 235 (227); 14.7 % | 8 (8) |
| `itur-hm` | 0.0016 | -37.9 | 5.0290 / 5.0716 / 5.1022 | 4501 (4539.1); 3.3 % | 24 (24) |
| `itur-hd` | 0.0008 | -50.5 | 15.1006 / 15.2148 / 15.1578 | 13515 (13617.3); 1.9 % | 56 (56) |
| `itur-hd` (48 kHz) | 0.0026 | -57.5 | 15.2190 / 15.2148 / 15.3167 | 13621 (13617.3); 1.9 % | 336 (336) |

Key. "rms width (standard)": the measured rms width, and the standard's
spread / 2. "error": the difference between them, relative. "shape (noise
floor)": the rms dB error, and what a true Gaussian measures with that many
Welch segments. "KS": the Kolmogorov distance to the Rayleigh distribution.
"Rice": the closed form for spread / 2. "MATLAB": `comm.RayleighChannel` on
the same seed and record. "fades counted (expected); gate": the count in the
record, the Rice expectation, and the relative tolerance. "echo": the delay in
samples the tool reports and the impulse test finds, and
ceil(delay × fs / 1000). "K factor": the Rice K factor, the power of the mean
over the mean power.

Legacy presets:

| preset | cutoff / delay | record, seed | rms width, Hz (= spread) | beyond the cutoff (Butterworth 21.95 %) | beyond 3 sigma (Butterworth 1.11 %) | shape, dB rms | KS to Rayleigh | fades/s: model / Rice for the measured width | fades counted; gate | delay line, samples |
|---|---|---|---|---|---|---|---|---|---|---|
| `good` | 0.1 Hz / 0 ms | 4500 s, 21 | 0.0987 (a 0.20 Hz spread) | 20.65 % | 1.153 % | 1.98 | 0.0083 | 0.0992 / 0.1001 | 446; 10.5 % | one path, no delay |
| `moderate` | 1 Hz / 1 ms | 900 s, 22 | 0.9735 (a 1.95 Hz spread) | 20.89 % | 1.207 % | 1.78 | 0.0087 | 0.9732 / 0.9874 | 871; 7.5 % | 7 (one short of 8) |
| `poor` | 1 Hz / 2 ms | 900 s, 23 | 0.9994 (a 2.00 Hz spread) | 21.96 % | 1.150 % | 1.75 | 0.0096 | 1.0067 / 1.0137 | 901; 7.4 % | 15 (one short of 16) |
| `flutter` | 10 Hz / 2 ms | 900 s, 24 | 9.8477 (a 19.70 Hz spread) | 21.83 % | 1.164 % | 1.72 | 0.0010 | 9.8313 / 9.9887 | 8799; 2.4 % | 15 (one short of 16) |
| `mpg` | 0.1 Hz / 0.5 ms | 4500 s, 25 | 0.0981 (a 0.20 Hz spread) | 21.63 % | 1.132 % | 1.81 | 0.0065 | 0.0999 / 0.0995 | 449; 10.5 % | 3 (one short of 4) |
| `mpp` | 1 Hz / 1 ms | 900 s, 26 | 0.9849 (a 1.97 Hz spread) | 21.44 % | 1.156 % | 1.81 | 0.0060 | 0.9743 / 0.9990 | 872; 7.4 % | 7 (one short of 8) |
| `mpd` | 2 Hz / 2 ms | 900 s, 27 | 1.9604 (a 3.92 Hz spread) | 21.45 % | 1.144 % | 1.77 | 0.0039 | 1.9207 / 1.9884 | 1719; 5.3 % | 15 (one short of 16) |

Key. "cutoff": the Butterworth 3 dB frequency the preset was named for. "rms
width (= spread)": measured; a Butterworth's rms width equals its cutoff, so
the spread in the standard's sense is twice the nominal number. "beyond the
cutoff": the power beyond the nominal frequency, which for a Butterworth is
one rms width; the CSV column is `tail2sigma_pct` for every row. "Rice for the
measured width": the closed form for the measured rms width; the legacy rows
are gated against it. "delay line": the samples the tool reports; the legacy
line is one sample shorter than ceil(delay × fs / 1000), as it was released.
No MATLAB reference is given: `comm.RayleighChannel` has no Butterworth
spectrum.

All 23 rows pass their gates (`pass` = 1 in the CSV).

### C and Go tests

`tests/common/test_watterson.c` has 17 tests. Three are kept from 1.9.18:

- Fading is finite and in range at 8 to 96 kHz.
- A single static path is rate-invariant.
- A pinned seed reproduces a fixed realisation (golden hash).

Fourteen are new in this change:

- The Gaussian tap has unit power and the standard's width. The
  autocorrelation at tau = 1 / (pi × spread) equals exp(−1/2).
- The Gaussian tap at 48 kHz is the same process as at 8 kHz.
- The delay line delivers the configured delay, and one sample short on the
  legacy path.
- Every preset delivers its delay through the preset itself: an impulse
  comes out at ceil(delay × fs / 1000) computed from the preset's own
  `delay_ms`, at 8 and 48 kHz, one sample short on the legacy presets; a
  two-path preset has equal power and equal spread on both paths. There is
  no copy of the preset table outside `watterson.c`.
- The spectrum tails separate the Gaussian from the Butterworth (the gate
  above).
- The −10 dB fade rate matches the Rice closed form (the gate above).
- The FFT behind the spectrum test puts a tone in its bin (a self-check).
- `watterson_add_path()` rejects out-of-range arguments.
- The Gaussian taps are zero-mean, circular, independent between paths and
  Rayleigh.
- A two-path preset keeps unit average power and the Rayleigh depth
  distribution.
- A fading preset reports the same SNR3k as one static unit-gain path,
  within 0.1 dB (`ccir-flutter`, 1200 s).
- A per-path frequency offset rotates at the set rate.
- A legacy preset is byte-identical to the hand-built legacy channel.
- A Gaussian preset is byte-identical to the hand-built Gaussian channel.

Status on macOS arm64 (Apple clang): 16 of 17 pass.
`test_a_pinned_seed_reproduces_a_fixed_realisation` fails with 0xF0EC0E2F
against the pinned 0x4D5ADF92. It fails with the same value at release 1.9.18
(f609b8f) and on this branch, at -O0 and with -ffp-contract=off, so the
mismatch predates this change. Its cause is not established. The pinned value
is the one the Linux continuous-integration (CI) hosts check, and there it
holds: the upstream CI jobs replayed in Debian 13 containers (gcc 14.2.0)
pass all 17 tests on amd64, and all 17 under AddressSanitizer and
UndefinedBehaviorSanitizer. On i386 the test skips itself on purpose, because
that target computes in extended precision (`FLT_EVAL_METHOD != 0`), and the
other 16 pass. The test's own comment records the macOS result.

`tests/integration/watterson_tool_test.go`: 7 tests, all pass (go 1.27.2,
darwin/arm64):

- `--list-presets` exits 0 with every name on stdout.
- `--help` exits 0; no arguments at all exits 1.
- Twelve wrong command lines exit non-zero, write nothing to stdout and
  create no output file. The cases are:
  - `--itur-m`: an ambiguous abbreviation.
  - `--ccir-f`: a unique abbreviation; names must be typed in full.
  - `--bogus`: an unknown option.
  - `--preset bogus`: an unknown preset name.
  - `--ccir-poor --legacy`: `--legacy` with a standard preset.
  - `--legacy --ccir-poor`: the same, in the other order.
  - `--poor --path 4,1,0,0.3`: a legacy preset mixed with a Gaussian path.
  - `--poor --good`: two presets.
  - `--path 0,1e-8,0,0.5`: a spread below the floor.
  - Five `--path` entries: the limit is `WATTERSON_MAX_PATHS`, 4.
  - `--Fs 0`: a bad sample rate.
  - One file name instead of two.
- Every name `--list-presets` prints is accepted as `--NAME`.
- `--Fs 48000` before and after `--ccir-poor` give the same bytes, print
  `Fs=48000` and place the echo at 96 samples.
- The path report shows the realised delay (0.3 ms → 3 samples = 0.375 ms;
  legacy `poor` 15 samples = 1.875 ms) and the spectrum.
- The bench preflight accepts `ccir-poor` and rejects `itur-m`, `bogus` and
  `ccir-f` with the tool's message.

One bench run on a standard preset (`MERCURY_CH_ENGINE=watterson
MERCURY_CH_FADING=ccir-poor MERCURY_CH_NO=-20`, default 102-byte payload,
2026-10-12) connected and delivered 102 of 102 bytes. That one run took about
13 s. The channel process's report (seed, `Path 1: delay=2.000 ms (16 samples
= 2.0000 ms) doppler=1.00 Hz spectrum=gaussian`) now appears in the test log.

### Legacy identity

Checked on 2026-10-12 against the 1.9.18 tool built from f609b8f (the
Watterson sources of 1.9.17, 1.9.18 and 1.9.19 are identical). Input: the 20 s 1 kHz
tone at 8 kHz, amplitude 8000, that the `python3` command in Reproduce
writes; `MERCURY_WATTERSON_SEED=7`; `--No -20`.

- `--good`, `--moderate`, `--poor`, `--flutter`: 320 000 of 320 000 output
  bytes identical to the old tool.
- `--legacy --path 0,1,0,0.7 --path 2,1,0,0.7` against the old tool's
  `--path 0,1,0,0.7 --path 2,1,0,0.7`: identical.
- the same two `--path` entries without `--legacy`: 317 664 of 320 000 bytes
  differ on that tone, because the tool now builds the standard channel for
  them. The count depends on the input; the commands in Reproduce give it.
- the old tool does not know `--mpg`, `--mpp`, `--mpd`: it prints
  "unrecognized option" and exits 0 with an empty output file. So those three
  cannot be compared between the two tools. `chanutil` has always built them from its
  own hard-coded paths. The test
  `test_legacy_preset_is_byte_identical_to_the_hand_built_legacy_channel`
  shows that a preset builds the same draws, in the same order, as the
  hand-built legacy channel.

So the channel statistics behind the Watterson-engine comparisons in
`docs/FADE-CLIFF-DECISION.md` section 2 and the Watterson rows of
`docs/MFSK-PORT.md` (MFSK: multiple frequency-shift keying) are unchanged,
and those comparisons are still valid. The FADE-CLIFF runs were unseeded (the
document states this), so they cannot be replayed sample for sample on either
version.

### Figures

Both figures were drawn on 2026-10-12 by the MATLAB script of the one-off
comparison; the script is not in the repository. It used the same functions,
seeds and record lengths as the comparison, so every number in a figure is a
number in `validation.csv` or `fade_rate.csv`. The subtitles were built from
the measured values. The figures are kept as data.

- `channel-figures/doppler_spectrum.svg`: the measured Doppler spectrum of the
  legacy `poor` (seed 23) and of `ccir-poor` (seed 9), 900 s records, against
  the standard's Gaussian (spread 1 Hz = 2 sigma). The spectra are drawn with
  their peaks at 0 dB. Subtitle values: legacy `poor` 22 % of the power beyond
  1 Hz and rms width 1.00 Hz; `ccir-poor` 4.5 % and 0.51 Hz.
- `channel-figures/fade_rate.svg` and `fade_rate.csv`: fades below −10 dB per
  second at the four CCIR points. The four values per point are the legacy
  preset at its own cutoff, the standard preset, MATLAB and the Rice closed
  form. The CSV has a header row
  and the columns `standard_preset, spread_hz, legacy_preset,
  legacy_cutoff_hz, seconds_legacy, seconds_standard, seed_legacy,
  seed_standard, fades_per_s_legacy, fades_per_s_standard,
  fades_per_s_matlab, fades_per_s_closed_form, n_fades_legacy,
  n_fades_standard, n_fades_matlab`. Its values:

| CCIR point | legacy preset (cutoff) | legacy fades/s (N, record, seed) | standard preset | standard fades/s (N, record, seed) | MATLAB (N) | Rice | legacy / standard |
|---|---|---|---|---|---|---|---|
| Good 0.1 Hz | `good` (0.1 Hz) | 0.0992 (446, 4500 s, 21) | `ccir-good` | 0.0514 (231, 4500 s, 7) | 0.0487 (219) | 0.0507 | 1.93 |
| Moderate 0.5 Hz | `moderate` (1 Hz) | 0.9732 (871, 900 s, 22) | `ccir-moderate` | 0.2503 (224, 900 s, 8) | 0.2722 (245) | 0.2536 | 3.89 |
| Poor 1 Hz | `poor` (1 Hz) | 1.0067 (901, 900 s, 23) | `ccir-poor` | 0.5151 (461, 900 s, 9) | 0.4889 (440) | 0.5072 | 1.95 |
| Flutter 10 Hz | `flutter` (10 Hz) | 9.8313 (8799, 900 s, 24) | `ccir-flutter` | 5.0547 (4524, 900 s, 10) | 5.07 (4563) | 5.0716 | 1.94 |

The legacy presets fade 1.9 to 3.9 times as fast as the standard presets of
the same name. The 3.9 is `moderate`: its 1 Hz cutoff is twice the spread of
CCIR Moderate (0.5 Hz), so it has both the width error and the preset error
together.

### Throughput on the old and the standard channels

The point of the standard presets is that a Mercury number can be put next
to a published number for another modem. This section does that once, with
figures the maintainers can reproduce, and with the old presets beside the
new ones so the difference is visible.

**Method.** `TestMercuryARQTransfer` over the `-x sock` bench, Watterson
engine, two Mercury stations, paced in real time. The payload is
incompressible (`MERCURY_TEST_PAYLOAD_RANDOM=1`, a fixed pseudo-random byte
sequence; the harness's default repeated string compresses to a few frames
and would overstate the rate). Throughput = payload bytes / virtual time of
the sending station's last key-up. The SNR of a point is the bench's measured
in-burst SNR3k on the payload direction (the SNR while the signal is present,
in 3 kHz), not the noise setting. Seven channels: AWGN, the standard
`ccir-good`, `ccir-moderate`, `ccir-poor`, and the legacy `good`, `moderate`,
`poor`. Two transfer sizes: 4 KB (noise −30 to −8 dB/Hz in 2 dB steps, seeds
1 and 2, 168 runs) and 32 KB (−40 to −22 dB/Hz, seed 1, 70 runs, plus 8 runs
at −20 and −14 dB/Hz on Good and Poor; 257 runs in all with the six
cross-check runs and the five reruns described below). The smaller transfer measures the
time to deliver a short message, which the ladder's climb dominates; the
larger one approaches the steady rate of the rung the channel allows. Data:
`channel-figures/throughput_old_vs_standard.csv` (one row per run: channel,
noise, payload, seed, result, wall time, link time, key-ups, in-burst SNR,
the channel tool's whole-run SNR, bytes per second, host, set). The two SNR
columns are both the channel tool's measurement: `snr3k_reported` is its
average over every sample of the run, silence included, and
`snr3k_inburst` is that figure corrected for the sender's transmit duty
cycle (`tests/integration/socksim_test.go`, `MeasuredSNR3k`). Their
difference is the duty cycle in decibels, 0.7 to 2.5 dB here. Neither
column is the modem's own SNR estimate; the harness does not record it. Runs were made on a
macOS laptop and in a Debian container on a Linux server; six strong-signal
points were repeated on both (set `xcheck`): AWGN and Good agree to 0.2 %
between hosts, the fading channels at 25 dB vary by up to 50 % between
repeats on either host (below).

Runs that did not deliver. At in-burst SNR below 1 dB, 18 of the 168 4 KB
runs did not finish inside the harness's floor (5 minutes plus the payload
at 5 bytes per second): the link was up on the MFSK rung at about 4 bytes
per second, so those cells read "below 4". Separately, 14 of the 78 32 KB
runs made on the Linux server, twelve at a time, stopped making progress on
every kind of channel including AWGN and at every signal level from −1 to
25 dB in-burst: both stations kept exchanging a short frame every 68 s until
the test's time limit (40 minutes for the first 35 runs, the harness's floor
of 114 minutes for the later 43). Their rows read `fail` with no rate. One
of them (AWGN, −30 dB/Hz, seed 1) rerun alone on macOS with verbose logs
completed in 146 s, and none of the 70 runs made on macOS stalled, so the
stall depends on timing, not on the channel, the signal level or the
payload; its cause is not established and it is recorded as an open
observation outside this change. Where a stalled run is the only run at its
point, that point is missing from the table below; the table never
interpolates across a stall. After the review, five of the stalled cases
(AWGN −30, `ccir-good` −20, `ccir-moderate` −40, `good` −36 and `moderate`
−34 dB/Hz, seed 1) were rerun one at a time on the same Linux host, with
verbose logs: all five completed, in 122 to 992 s of link time. They are in
the data file as set `solo` and are not used in the table, whose method is
the twelve-at-a-time run. So the stall comes from twelve real-time-paced
daemon pairs sharing one host; what inside the modem or the harness gives
way under that load is not established.

**Mercury on the standard channels, bits per second, interpolated to the SNR
points of the published comparison** (log-linear between the two nearest
bench points of the same channel; one seed for 32 KB, two for 4 KB):

| modem, channel | 0 dB | 5 dB | 10 dB | 15 dB | 20 dB | 25 dB |
|---|---|---|---|---|---|---|
| Mercury, standard Good (`ccir-good`), 32 KB | – | 300 | 686 | 1386 | 1849 | – |
| Mercury, standard Good, 4 KB | 86 | 275 | 649 | – | – | – |
| Mercury, legacy `good` (one path, 0.1 Hz), 32 KB | – | – | 765 | 1347 | 1826 | – |
| Mercury, legacy `good`, 4 KB | 42 | 284 | 803 | – | – | – |
| Mercury, standard Moderate (`ccir-moderate`), 32 KB | – | – | 1002 | 956 | – | – |
| Mercury, standard Moderate, 4 KB | 44 | 378 | 632 | 679 | – | – |
| Mercury, legacy `moderate` (1 ms, 1 Hz cutoff), 32 KB | – | – | 994 | 866 | 928 | – |
| Mercury, legacy `moderate`, 4 KB | 55 | 250 | 805 | 672 | – | – |
| Mercury, standard Poor (`ccir-poor`), 32 KB | 141 | 409 | 888 | 912 | 944 | 944 |
| Mercury, standard Poor, 4 KB | 80 | 486 | 802 | – | – | – |
| Mercury, legacy `poor` (2 ms, 1 Hz cutoff), 32 KB | 162 | 552 | 932 | 915 | 1242 | – |
| Mercury, legacy `poor`, 4 KB | 127 | 388 | 614 | 648 | – | – |
| Mercury, AWGN, 32 KB | – | – | 1587 | 2155 | 2091 | – |
| Mercury, AWGN, 4 KB | 292 | 799 | 877 | – | – | – |
| VARA HF 2300, CCIR Poor (N5TW 2020) | 130 | 200 | 490 | 950 | 1450 | 2330 |
| PACTOR-4, CCIR Poor (N5TW 2020) | 160 | 470 | 1330 | 2000 | 2930 | 3200 |
| VARA HF 2300, CCIR Good (N5TW 2020) | 150 | 170 | 400 | 870 | 1570 | 2590 |
| PACTOR-4, CCIR Good (N5TW 2020) | 150 | 290 | 1010 | 2070 | 2880 | 3810 |
| VARA HF 2300, AWGN (N5TW 2020) | 190 | 310 | 710 | 1510 | 3150 | 4990 |
| PACTOR-4, AWGN (N5TW 2020) | 230 | 530 | 2110 | 3240 | 5330 | 5400 |

"–" means no bench point on both sides of that SNR for that channel and
size. The Mercury rows come from `throughput_old_vs_standard.csv`; the six
published rows are read from the charts (about ±5 %) of T. Whiteside N5TW,
"A comparison of Winlink digital mode performance ... Teensy IONOS
Simulator, Take Three", 2 November 2020 (winlink.org). That test's MPG and
MPP channels are the CCIR 520-2 Good (0.5 ms, 0.1 Hz) and Poor (2 ms, 1 Hz)
definitions, two equal Rayleigh paths, with the SNR in a 3 kHz noise
bandwidth: the same channels and the same SNR convention as the standard
presets here. Its WGN rows (pages 4, 11 and 13 of that report) are the AWGN
rows here.

What the table says:

- On AWGN Mercury reaches its 16-QAM rung and stays at about 2.1 kbps from
  13 dB up (seven exchanges for 32 KB). VARA keeps rising to 3150 bps at
  20 dB and 4990 at 25 dB on AWGN, and PACTOR-4 reaches 5330 at 20 dB:
  above 13 dB the gap is the modulation set, one 16-QAM rung in 2.1 kHz
  against 32-QAM in 2.3 kHz. Below 10 dB on AWGN Mercury is ahead of both
  (4 KB: 292 bps at 0 dB against 190 and 230; 799 at 5 dB against 310 and
  530).
- On the standard Good channel Mercury is ahead of VARA's 2020 figures from
  5 to 20 dB (300 to 1850 bps against 170 to 1570) and behind PACTOR-4 from
  10 dB up. At 0 dB VARA is ahead (150 against about 80).
- On the standard Poor channel Mercury is ahead of VARA from 5 to about
  12 dB (409 to 888 against 200 to 490), level at 15 dB, and behind above
  that: Mercury stays near 900 bps while VARA reaches 1450 and 2330. The
  cause is the ladder, not the channel: a verbose run at 25 dB on Poor
  reached the 16-QAM rung after 45 s, but on this fast-fading channel the
  probes to the top rungs succeed or fail by timing, and four repeats of the
  same seed at 25 dB gave 857, 945, 1360 and 1379 bps. One seed cannot
  resolve the Poor channel above 15 dB; the 944 in the table is one run.
- The legacy preset with the same name gives a Mercury number within that
  scatter of the standard one at every SNR measured. The standard presets
  change what the number can be compared with, not the number itself.

Caveats, so the comparison is not read as more than it is: N5TW moved 120 kB
through Winlink Express, whose compression state is not stated, with 2300 to
2400 Hz modems against Mercury's 2.1 kHz top mode; the test was tuned with
the PACTOR vendor's help; its values are chart readings; Mercury's are one or
two seeds per point and interpolated; and the fading-channel figures above
15 dB carry the probe scatter just described.

### Which document was measured on which engine

This matters for the follow-up that re-measures thresholds on the standard
presets. The thresholds of each document were measured on a different
channel, so each will move by a different amount.

- `docs/FADE-CLIFF-DECISION.md`, section 2 (Watterson A/B): the Watterson
  engine with the legacy `moderate` and `poor` presets (Butterworth taps,
  faster than the standard channels of those names); unseeded runs.
- `docs/FADE-CLIFF-DECISION.md`, section 4 (NVIS): pathsim,
  `--midlat-dist-nvis`; not this model.
- `docs/MFSK-PORT.md`: the main sensitivity tables are codec2 `ch` MPP (2 ms,
  Gaussian spectrum). The rows named "Watterson 2 ms/1 Hz" and "Watterson
  2 ms/0.2 Hz" (sync threshold, interleaving, the modem73 cross-check) were
  run on the old tool. There every path had Butterworth taps. 2 ms/0.2 Hz is
  not a preset, so it was a `--path`. To rebuild such a row today, pass
  `--legacy` before `--path`. The section "…and it loses under fading" uses
  `chanutil`'s `mpp`, the legacy 1 ms, 1 Hz cutoff channel. It compares that
  DATAC16 column with the MPP table of `docs/MODES.md`, which is `ch`'s MPP
  (2 ms, Gaussian). Those are two different channels under one name.
- `docs/MODES.md`: codec2 `ch` (`ch --mpp --fading_dir unittest`), not this
  model. Its Doppler width was not measured in this change.

## Reproduce

```sh
make && make -C utils watterson_test && make -C tests test_watterson && ./tests/test_watterson
utils/watterson_test --list-presets
utils/watterson_test in.s16 out.s16 --ccir-poor --No -20            # the standard channel
utils/watterson_test in.s16 out.s16 --poor --No -20                 # the legacy channel, unchanged
utils/watterson_test in.s16 out.s16 --legacy --path 0,1,0,0.7 --path 2,1,0,0.7 --No -20   # the old tool's --path
# the input used for the legacy identity check: a 20 s 1 kHz tone at 8 kHz, amplitude 8000, int16
python3 -c "import struct,math; open('in.s16','wb').write(b''.join(struct.pack('<h', round(8000*math.sin(2*math.pi*1000*i/8000))) for i in range(160000)))"
# legacy identity against the 1.9.18 tool, built from f609b8f (same for --good, --moderate, --flutter):
MERCURY_WATTERSON_SEED=7 utils/watterson_test in.s16 new.s16 --poor --No -20
MERCURY_WATTERSON_SEED=7 /path/to/1.9.18/utils/watterson_test in.s16 old.s16 --poor --No -20
cmp new.s16 old.s16                                                  # identical
# the old tool's --path pair against the new tool with and without --legacy:
MERCURY_WATTERSON_SEED=7 /path/to/1.9.18/utils/watterson_test in.s16 old_path.s16 --path 0,1,0,0.7 --path 2,1,0,0.7 --No -20
MERCURY_WATTERSON_SEED=7 utils/watterson_test in.s16 new_legacy.s16 --legacy --path 0,1,0,0.7 --path 2,1,0,0.7 --No -20
MERCURY_WATTERSON_SEED=7 utils/watterson_test in.s16 new_path.s16 --path 0,1,0,0.7 --path 2,1,0,0.7 --No -20
cmp old_path.s16 new_legacy.s16                                      # identical
cmp -l old_path.s16 new_path.s16 | wc -l                             # 317664 differing bytes on this tone
# the tool's command line and the bench preflight:
cd tests/integration && go test -count=1 -run TestWatterson -v .
# the bench on a standard channel:
MERCURY_CH_ENGINE=watterson MERCURY_CH_FADING=ccir-poor MERCURY_CH_NO=-20 go test -count=1 -run 'TestMercuryARQTransfer$' -v .
```

Notes for anyone measuring the channel with their own commands:

- `./tests/test_watterson` exits 1 on macOS because of the pinned-hash test
  described above; the other 16 tests pass. In the Debian 13 CI containers
  (amd64, and under the sanitizers) the pin holds and all 17 pass.
- `--No -100` is not noise-free. The tool turns its AWGN off only below
  −150 dB/Hz; at −100 dB/Hz the complex noise variance is fs × 1e-4. Fades
  must be counted on a band-limited, decimated envelope, as the one-off
  comparison did (200 Hz lowpass, then decimation to 64 × the spread). Counted on the
  demodulated 8 kHz samples without the lowpass, the same record gives orders
  of magnitude more crossings (over a million for `ccir-moderate`, seed 8).
  With the lowpass alone it gives about 15 % more than after decimation.
  Those two counts were measured once while writing that script; they are
  not in the CSV.
- Run length. A tone on a standard preset crosses −10 dB downward
  0.507 × spread times per second: once per 20 s at 0.1 Hz, once per 2 s at
  1 Hz. A short run on `ccir-good` or `itur-mq` is a small sample: 100 s holds
  about 5 deep fades. The count's one-sigma scatter is about 0.74 sqrt(N).
  F.1487 also states a minimum test duration; its text was not available when
  this record was written, so the rule is not quoted here.
- A seed names one fade only for one configuration (spectrum, paths, sample
  rate, AWGN on or off); see "Seeds" above.

## Follow-ups (not in this change)

- Re-measure the mode thresholds and the carousel's fading figures on the
  standard presets, by engine. The Watterson-engine rows (FADE-CLIFF-DECISION
  section 2, the MFSK-PORT Watterson rows) were on Butterworth taps, faster
  than the standard. The `ch` rows (MODES.md) were on a 2 ms Gaussian channel
  whose width was not measured here. The pathsim rows (FADE-CLIFF-DECISION
  section 4) are a third engine.
- An impulsive (atmospheric) noise model with parameters derived from ITU-R
  P.372. Its amplitude distribution would be gated the way this change gates
  the Doppler spectrum, with SNR still referenced to 3 kHz.
- Receiver AGC with attack and decay, transmitter ALC, realistic transmitter
  and receiver filter shapes, transmitter-receiver sample-rate offset and
  time-varying frequency drift. These stay outside the channel presets so
  that standard-channel numbers stay comparable.
- Frequency-shift presets and an integration case under carrier offset at
  Mercury's own ±50 Hz and ±100 Hz. The ±50 Hz is the acquisition range of
  the OFDM (orthogonal frequency-division multiplexing) modem, `fmin` and
  `fmax` in `modem/freedv/ofdm.c`. The ±100 Hz is the MFSK acquisition
  tolerance given in `docs/MFSK-PORT.md`. Today a per-path shift is reachable
  only through `--path` or the C API. Every sensitivity so far was measured
  on a perfectly tuned simulator.
- A separate seed-derived random stream for the AWGN, so that a seed names
  the fade regardless of `--No` and sample rate. The golden hash, which runs
  with AWGN on, would then be re-pinned.
- `chanutil_open()`: seed the channel before `add_paths` as well, so the
  legacy 0.1 Hz presets are reproducible from the explicit seed. The existing
  call must stay, or every other explicit-seed realisation changes.
- A `--tapdump FILE` option that writes the pre-noise complex taps per path,
  for re-measurement without the Hilbert, int16 and AWGN path.
- Print the expected fade count (closed form) in the tool's summary and in the
  bench's exit line. A short run on a slow preset is then labelled as the
  small sample it is.
- Fractional-delay interpolation for custom delays at rates such as 9600 or
  44100 Hz, where a preset delay is not a whole sample (MATLAB interpolates).
  Every rate the project uses is exact, and `ceil` must stay for the legacy
  pinned realisation.
- Document transmitter headroom and have the bench log the tool's clipping
  summary.
- Longer records or multi-seed averaging, only if statements finer than 2 %
  on the width or 5 % on the fade rate are wanted.
