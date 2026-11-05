# Frequency offset and the acquisition search: what the search can capture

Measured 2026-11-04 on `mercuryv2` at `1c2824a0` (v1.9.23 development), after
the PR #455 review, with the receiver unsync fix (#457) in place.

## Answer

**A ±20 Hz baseband offset costs DATAC16 almost nothing, and the earlier claim
that it destroys acquisition was an instrument bug.** The acquisition can
capture a carrier mistuned by **±55 Hz**; beyond that the acceptance metric
collapses and the modem does not acquire at all. That window is the *search's*
geometry, not the threshold's, and it is the same on DATAC15.

The number that matters for a real station: **the capture window is ±55 Hz,
and a split TX/RX pair whose references differ by 10 ppm at 14 MHz puts the
carrier 140 Hz off** — the modem would not acquire it at all. Widening the
search's span from ±50 Hz to ±150 Hz captures ±153 Hz for **2.5× the idle
CPU** (3.5% -> 8.6% of a core on x86, DATAC16, 600 s of noise, threshold
0.40, still **zero false syncs** in those 600 s). That is a change to their
code and their call, not ours; the measurements are below.

The first half of this document is a correction: the offset numbers in
[ACQ-TUNING-FINDINGS.md](ACQ-TUNING-FINDINGS.md) were measured with a broken
instrument, and that document's offset section has been corrected in place.

## The instrument was wrong (three defects)

`utils/bench_freq_shift_real` is the analytic shift that models a mistuned
SSB radio. It had three defects, all now fixed, and the first one alone
invalidates every offset number measured with it:

1. **6 dB of signal.** The shift took the positive half of the DFT and mixed
   it up, without doubling it into an analytic signal — so the "shifted"
   burst came out at **half amplitude** (-6.02 dB, measured). The sweep
   calibrates its AWGN *before* the shift, so every offset row was
   **6 dB below its labelled SNR**. At a nominal -9 dB the modem was really
   being asked to acquire at -15 dB. This alone explains the old result: a
   20 Hz offset "destroyed" acquisition because the instrument had already
   spent the margin.
2. **A noise floor from uninitialised memory.** The shifted spectrum's upper
   half was never written (the buffer is `malloc`ed, and only the lower half
   is filled), so every shifted burst carried broadband garbage at about
   -24 dB. It was zero in practice on a fresh heap, but it was luck, not
   arithmetic.
3. **A phase reference at the padded buffer's start**, not the burst's, so
   the shifted burst carried a constant phase of `2*pi*f*4096/fs` (90° at
   20 Hz). Harmless to a magnitude-based modem, but it made the instrument
   impossible to check against a closed form.

`utils/shift_selftest.c` is new and checks the instrument against a pure
tone, whose shifted form is known exactly: output frequency, output
amplitude, sample-for-sample agreement with the closed form, and the offset
actually applied versus the offset asked for (the shift moves whole FFT
bins, so it is quantised to `fs/nfft` — 0.24 Hz at DATAC16's burst length).
All four checks pass now; the first two failed before.

## The corrected numbers

**±20 Hz, DATAC16, the rows the old document published** (50 trials per
point, AWGN, threshold 0.40, 12/32 UW budget, v1.9.22 code, corrected
instrument):

| SNR | 0 Hz | +20 Hz | -20 Hz |
|---|---|---|---|
| -12 dB | 35/26 | 29/19 | 23/15 |
| -11 dB | 42/35 | 40/37 | 39/34 |
| -10 dB | 50/48 | 48/46 | 48/40 |
| -9 dB | 50/45 | 50/50 | 50/49 |

acquired/delivered per 50. The old document's 0.40 row was **0-1, 1, 1, 2-3**
acquired at these four SNRs. At -9 dB and above, **a 20 Hz offset is free**:
the same 50/50 acquired, the same deliveries. The cost shows only at the
fringe floor (-12 dB), where the metric is near the threshold and an offset
takes some of the margin with it.

**The capture window** (DATAC16, clean channel, 20 trials per point, offset
stepped 0.5 Hz near the edge):

| offset | acquired | delivered | mean timing_mx | estimate error |
|---|---|---|---|---|
| ±50.0 | 20/20 | 20/20 | 0.82 | <0.1 Hz |
| ±54.5 | 20/20 | 20/20 | 0.82 | -1.3 Hz |
| ±55.0 | 20/20 | 12/20 | 0.58 | -3.0 Hz |
| ±55.5 | 18/20 | 8/20 | 0.46 | -7.8 Hz |
| ±56.0 | 17/20 | 3/20 | 0.28 | -8.6 Hz |
| ±57.0 | 13/20 | 0/20 | 0.10 | -11.3 Hz |
| ±65.0 | 2/20 | 0/20 | - | -19 Hz |

So the window is **±55 Hz**, and the cliff is sharp: the estimate error
grows first (the fine stage's hypotheses run out), then the acceptance
metric collapses, then delivery stops. Past the cliff the "acquisitions" are
noise-driven false syncs — nothing is delivered.

DATAC15 behaves the same (10/10 to ±50 Hz, 3-4/10 at ±60), as the shared
geometry predicts.

## Why ±55 Hz, from the code

`modem/freedv/ofdm.c`, `burst_acquisition_detector()` — the path Mercury
actually runs (see the note on streaming below):

- the coarse stage sweeps the frequency hypotheses at **5 Hz** steps over
  **`ofdm->fmin`..`ofdm->fmax`**, which `ofdm_create()` hardcodes to
  **±50 Hz** (`ofdm.c:446`; the config's own `fmin`/`fmax` are overwritten
  there);
- the fine stage then searches **±3 Hz** around the winner at 1 Hz steps —
  the width is `ceil(fstep/2)`, so it covers the coarse grid's uncertainty;
- the capture window is therefore **the span plus about 5 Hz**: ±50 Hz of
  grid plus ±3 Hz of fine window plus the metric's tolerance at the edge.

The fine stage is what resolves the offset to better than 0.1 Hz, and it is
why the estimate error is ~0 across the whole window and only grows near
the edge.

**A note on which path this is.** Mercury's backend calls
`freedv_set_frames_per_burst()` (`modem/modem_freedv.c:28`), which calls
`ofdm_set_packets_per_burst()`, which sets `ofdm->data_mode = "burst"`.
So Mercury runs the **burst** acquisition path for every mode, including
DATAC15/16 — the `data_mode = "streaming"` in `ofdm_mode.c` is overridden.
The streaming path (`ofdm_sync_search_stream`, three hypotheses at
-40/0/+40 Hz with a ±20 Hz refinement) is inherited FreeDV/codec2 code and
is **not** Mercury's path. Its geometry is a different question, for codec2
and FreeDV rather than here.

## What a real link's offset is

Analysis only (no radio work was done for this document):

| setup | baseband offset at the modem |
|---|---|
| same radio, TX and RX, shared reference | < 1 Hz |
| separate TX and RX, separate references, 10 ppm at 14 MHz | **±140 Hz** |
| separate references, 0.1 ppm (a disciplined rig) | ±1.4 Hz |
| resampling in the audio path | the resampler's own ppm difference |

The RF carrier error between two radios appears *inside* the modem as a
baseband offset, because each radio converts to and from audio with its own
reference. So the ±55 Hz window is comfortable for a disciplined station and
fatal for a split pair with ordinary references: the modem would simply
never acquire, with no indication that frequency was the problem.

Whether that is worth fixing depends on how many of their stations run split
TX/RX with independent references — a question for them, not for us.

## The prototype: a wider span

**The knob already exists.** `freedv_set_tuning_range(f, fmin, fmax)`
(`freedv_api.c:1585`) sets `ofdm->fmin`/`fmax` for burst-mode OFDM, which is
what Mercury runs, and nothing in Mercury calls it. Widening the window in
production is therefore one call in `fdv_configure()`
(`modem/modem_freedv.c`), after `freedv_set_frames_per_burst()` has put the
modem in burst mode (the call returns 0 and does nothing before that).

`ofdm_set_freq_search_range()` (new, bench-facing) does the same thing
without the burst-mode gate, so the bench can set the span on any mode.
Everything else is stock.

| span | capture edge | idle CPU (x86, 600 s noise, 0.40) | false syncs |
|---|---|---|---|
| ±50 Hz (stock) | ±55 Hz | 3.5% (21.1 cpu s) | 0 |
| ±100 Hz | ±105 Hz | 5.7% (34.3 cpu s) | 0 |
| ±150 Hz | ±153 Hz | 8.6% (51.6 cpu s) | 0 |

The cost is linear in the span: one correlation pass per 5 Hz of span, on
every listening second. With the span at ±150 Hz the interior is solid —
**10/10 acquired and 10/10 delivered at every offset from -150 to +150** —
and at the fringe (-9 dB, 50 trials) offsets to ±100 Hz are free (49-50/50)
while ±140 Hz costs about half (28-29/50).

**A cheaper variant was tried and rejected.** Stepping the coarse grid more
coarsely (20 Hz instead of 5 Hz) over the same ±150 Hz span would cut the
CPU to 0.76× of stock, and it does widen the edge to ±157 Hz — but it breaks
acquisition at unpredictable offsets *inside* the range, including offsets
the stock build handles: ±20 Hz fell from 50/50 (the 50-trial fringe sweep)
to 3-4 out of 10 in the interior sweep, and -100 Hz to 2/10. The 5 Hz step is load-bearing: the coarse metric's sidelobe structure
means a coarser grid sometimes picks a hypothesis the fine window cannot
recover from. The stock step stays.

## What we propose

1. **Take the instrument fix** (`utils/bench_common.c` plus the new
   `utils/shift_selftest.c`). It corrects a measurement tool, and the
   numbers it produced are in this document and in the corrected
   ACQ-TUNING-FINDINGS.md.
2. **Decide the capture window.** ±55 Hz today, and widening it needs no
   new code: `freedv_set_tuning_range()` is already in the tree and unused. The prototype buys ±153 Hz
   for 2.5× the idle CPU on x86 (the Pi 4 number is theirs to measure, and
   the cost is the same shape there). If their stations do not meet split
   TX/RX with independent references, there is no case for it, and the
   honest answer is that the acquisition is fine as it stands.
3. **Say whether this is worth anything to their users.** We cannot see
   their issue tracker's frequency-offset reports; if users have been
   reporting "no sync" on split setups, the window is the answer.

## Caveats

- AWGN only, and simulation only. Fading confounds acquisition loss with
  payload loss (as in ACQ-TUNING-FINDINGS.md).
- The CPU numbers are x86. Their review asked for the Pi 4 number before
  #455 merged, and the same applies here: `utils/idle_cpu_probe` reports
  per audio-second so the two are directly comparable.
- The offset is applied to the *burst* as a whole, which models a radio
  that transmits and receives at a fixed frequency difference — a
  drifting offset (a moving path) is not covered.
- DATAC3 acquires 0/10 in this bench configuration *with or without* an
  offset, so nothing is claimed about it. DATAC4 and the DATAC15/16 family
  acquire normally.
- The fine stage's 1 Hz steps mean the frequency estimate is quantised to
  1 Hz before the decoder's tracking refines it; the tracked estimate is
  the accurate one.
- The cost model (one correlation pass per 5 Hz of span) is for the data
  modes, whose coarse search runs brute force. The FFT search path
  (`est_timing_and_freq_fft`) exists for the small modes and is not what
  DATAC15/16 use.

## Reproducing

```sh
make -C utils shift_selftest offset_acq_sweep idle_cpu_probe
# the instrument, before anything else:
utils/shift_selftest
# the capture window (clean channel, then the edge at 0.5 Hz steps):
utils/offset_acq_sweep DATAC16 20 -3 -70 70 5 0.40 0
utils/offset_acq_sweep DATAC16 20 -3 50 65 0.5 0.40 0
# the fringe, where the old claim was made:
utils/offset_acq_sweep DATAC16 50 -9 -40 40 10 0.40 0
utils/offset_acq_sweep DATAC16 50 -12 -30 30 10 0.40 0
# the prototype's wider span:
utils/offset_acq_sweep DATAC16 10 -3 -150 150 15 0.40 0 150
# its cost, and the false syncs it costs:
utils/idle_cpu_probe DATAC16 600 0.40 150
```
