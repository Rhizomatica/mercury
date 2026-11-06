# Acquisition windows: how far off carrier each mode can be captured

**Status:** dated record, 2026-11-05. The measurements below are
simulation-only (AWGN, the project's own bench) and every table
regenerates from the commands in "Reproducing" below.
**Author:** Nathan Goodall
**Scope:** the OFDM and MFSK modes' acquisition capture windows, the
channel-model capability that makes them a session-level question, and the
measurement discipline behind them.

## The question

The previous record (`docs/OFFSET-ACQUISITION-FINDINGS.md`) measured the
DATAC16 acquisition's capture window at **±55 Hz** and found that a ±20 Hz
offset is nearly free.  It also said, in its "proposals" section, that the
window is a mode-level question it could not answer with one bench:

> The capture window is set by the search's geometry, and the geometry is
> per mode: the OFDM modes' search spans ±50 Hz with a 5 Hz grid and a ±3 Hz
> fine stage, while MFSK has its own 13-hypothesis search over ±94 Hz.

This record answers that: **every mode's window**, measured the same way,
at the mode's own operating point.  And it puts the channel model's
carrier offset in place, so the window becomes something a session can be
run over.

## The answer

**The window is the same for every OFDM mode, and it is wider for MFSK.**

| | capture window | what sets it |
|---|---|---|
| DATAC0/1/3/4/13/14/15/16/17, QAM16C2 | **±55 Hz** | the search's geometry: a 5 Hz coarse grid over a ±50 Hz span (`ofdm.c:446`), refined ±3 Hz around the winner |
| MFSK, MFSK500 (the floor) | **±100 Hz** | its own search: 13 preamble hypotheses spanning ±94 Hz in half-bin steps (`docs/MFSK-PORT.md`) |

The span is a **modem-level constant**, not a per-mode one: `ofdm_create`
sets `ofdm->fmin = -50` and `ofdm->fmax = +50` once
(`modem/freedv/ofdm.c:446-447`) and nothing in `ofdm_mode.c` overrides
them, so every OFDM mode searches the same range with the same grid.  That
is the whole reason the windows agree.

The consequence is the operationally interesting part:

- **A station 80 Hz off can still connect** -- on the MFSK floor, because
  the OFDM modes cannot capture it and the floor can.
- **A station 140 Hz off cannot connect at all**, at any SNR: 140 Hz is
  what two radios with references 10 ppm apart at 14 MHz differ by, and it
  is outside *both* windows.  The operator sees "no sync" and nothing in
  the logs points at frequency.

## The measurement, and what it takes to believe it

### The instrument, first

A capture-window number is worth exactly as much as the instrument that
produced it, and this project's instrument had already been caught out
once: the analytic shift in `utils/bench_common.c` came out 6 dB down
(half amplitude), which made every offset row sit 6 dB below its labelled
SNR and produced a confidently wrong answer in the previous record.

So the window is measured with the fixed instrument, and the instrument has
its own self-test (`utils/shift_selftest.c`, the closed-form tone check).
Nothing in this record is reported without it.

### Calibrate before measuring

Every number here sits next to a published one, and the bench reproduces
the published ones first:

| published number | where | reproduced? |
|---|---|---|
| DATAC16's acquisition table: −8 dB 100/99, −9 99/98, −10 84/78, −11 52/48, −12 26/19, −13 7/2 | `docs/MODES.md` | **yes, exactly** at threshold 0.45 |
| the #455 calibration row: 0.45 → 11/32/44/48, 0.40 → 35/42/50/50, 0.35 → 50/50/50/50 | PR #455 | **yes, exactly** |
| MFSK's tolerance: ±12 Hz → ±100 Hz | `docs/MFSK-PORT.md` | **yes**: 10/10 to ±100 Hz, 2/10 at ±110, 0/10 at ±125, both signs |

The first row needed a tool change to be checkable: `acquire_vs_decode`
had no way to set the acquisition threshold, so the table recorded at the
stock 0.45 could not be reproduced after #455 moved DATAC15/16 to 0.40.
It takes an optional threshold argument now, and the table above is
reproducible with it.

### Each mode at its own operating point

A capture window is not a property of a mode alone; it is a property of a
mode *at a given SNR*.  The narrow modes live at very different SNRs, and
the first attempt to measure DATAC3's window at −12 dB found 0/10
acquisitions -- which is not an instrument fault at all: **DATAC3's knee
is at −4.8 dB SNR3k, not −9 dB like DATAC16**.  Measured below its own
floor, a mode looks as if it had no window at all.

So the window is measured at each mode's own **50% acquisition point**,
found by a sweep of its own (`acquire_vs_decode`, 50 trials a point):

| mode | bandwidth | 50% acquisition at | stock threshold |
|---|---|---|---|
| DATAC16 | 2000 Hz | −12.2 dB | 0.40 |
| DATAC15 | 200 Hz | −12.3 dB | 0.40 |
| DATAC13 | 200 Hz | −10.8 dB | 0.45 |
| DATAC14 | 250 Hz | −10.8 dB | 0.45 |
| DATAC4 | 250 Hz | −10.2 dB | 0.50 |
| DATAC0 | 500 Hz | −6.1 dB | 0.08 |
| DATAC3 | 500 Hz | −4.8 dB | 0.10 |
| DATAC1 | 1700 Hz | −4.7 dB | 0.10 |
| DATAC17 | 2100 Hz | −4.5 dB | 0.10 |
| QAM16C2 | 2100 Hz | +3.6 dB | 0.30 |

The spread is 16 dB, and it is not the mode's *robustness* that differs
so much as where the SNR3k axis lands for a mode of that bandwidth: the
narrow modes concentrate their power, so their SNR3k floor is much lower
than the wide ones'.

### The windows

Every OFDM mode, swept over the offset at its own operating point (10 or 20
trials a point; the rates are acquired/delivered):

| mode | operating point | acq at 0 | ±40 Hz | ±50 Hz | ±60 Hz | ±70 Hz | last offset acquiring |
|---|---|---|---|---|---|---|---|
| DATAC15 | −12.3 dB | 35% | 40/40% | 30/35% | 0/0% | 0/0% | ±50 Hz |
| DATAC16 | −12.2 dB | 45% | 55/45% | 50/35% | 10/0% | 0/0% | ±60 Hz |
| DATAC13 | −10.8 dB | 70% | 65/50% | 50/55% | 0/0% | 0/0% | ±50 Hz |
| DATAC14 | −10.8 dB | 50% | 30/35% | 35/45% | 0/0% | 0/0% | ±50 Hz |
| DATAC4 | −10.2 dB | 45% | 55/55% | 35/60% | 0/0% | 0/0% | ±50 Hz |
| DATAC0 | −6.1 dB | 60% | 35/45% | 60/70% | 0/0% | 0/0% | ±50 Hz |
| DATAC3 | −4.8 dB | 65% | 35/50% | 50/40% | 0/0% | 0/0% | ±50 Hz |
| DATAC1 | −4.7 dB | 20% | 50/20% | 30/30% | 0/0% | 0/0% | ±50 Hz |
| DATAC17 | −4.5 dB | 10% | 50/50% | 20/40% | 0/0% | 0/0% | ±50 Hz |
| QAM16C2 | +3.6 dB | 45% | 35/40% | 55/65% | 0/0% | 0/0% | ±50 Hz |

Three things to read off it:

- **The edge is the same for every OFDM mode**: the acquisition is still
  there at ±50 Hz, at a rate comparable to the mode's own rate at zero
  offset, and it is gone by ±60 Hz (DATAC16's 10% at ±60 Hz is the
  false-sync tail -- acquired, never delivered).  That is the ±55 Hz
  window, resolved to the sweep's 10 Hz grid, and it is the same for all
  ten modes because the span is a modem-level constant.
- **At the operating point the rate at the edge is already low**, and the
  delivered count collapses before the acquisition does.  A mode at its own
  50% point has nothing to spare: the window is the same, but the margin
  inside it is gone.  This is why the window matters most in the *healthy*
  case (the previous record's ±3 dB, where the window is ±55 Hz with
  everything delivered) and less at the fringe.
- **The rates are noisy** -- 10 or 20 trials a point, and the modes are
  measured at the point where their own rate is 50%.  The *edge* is
  consistent across ten modes and two signs; the rates inside it are not
  worth reading to the point.

### MFSK

Measured with the MFSK bench that was already in the tree
(`utils/mfsk_channel_sweep.sh`, which drives the real MFSK backend
through the channel tool), 10 trials a point, AWGN at −3 dB SNR3k:

| offset | 0 | ±50 | ±75 | ±94 | ±100 | ±110 | ±125 |
|---|---|---|---|---|---|---|---|
| MFSK (mode 100) | 10/10 | 10/10 | 10/10 | 10/10 | 10/10 | 2/10 | 0/10 |
| MFSK500 (mode 102) | 10/10 | 10/10 | 10/10 | 10/10 | 8/10 | 3/10 | 0/10 |
| MFSK500 (mode 103) | 10/10 | 10/10 | 10/10 | 10/10 | 10/10 | 0/10 | 0/10 |

So MFSK's window is **±100 Hz**, as `docs/MFSK-PORT.md` says, and the edge
is just past it.  The floor's window is nearly twice the OFDM modes'.

## The channel model: making it a session-level question

A window is a number until a link can be run over it.  The channel model's
follow-up list (`docs/CHANNEL-MODEL.md`) asks for exactly that -- "an
integration case under carrier offset at Mercury's own ±50 Hz and ±100 Hz"
-- and the model now carries the offset:

```c
watterson_set_carrier_offset(&w, hz);
```

It is the transmitter's frequency error as the receiver sees it, applied
before the paths, so the whole burst moves in the passband while the fade
stays what it was.  The tool's `--freq` uses it now (it used to carry its
own oscillator, a second copy of the arithmetic that drifted 0.02 Hz at
50 Hz), the benches reach it through `utils/chanutil_open_offset()` and
`utils/acquire_vs_decode`'s offset argument, and the integration harness
can be run over a mistuned link:

```sh
MERCURY_CH_ENGINE=watterson MERCURY_CH_FREQ=50 \
    go test -count=1 -run 'TestMercuryARQTransfer$' -v .
```

`MERCURY_CH_FREQ_FWD` and `MERCURY_CH_FREQ_REV` set it for one direction
only, because a real mistuned link is asymmetric: two radios with
independent references put the offset on one direction, and that is the
case the capture window is about.

### The two mechanisms agree

The window measured with the bench's own analytic shift and the window
measured with the channel model's rotation are the same window:

| offset | 0 | ±40 | ±50 | ±55 | ±60 | ±70 |
|---|---|---|---|---|---|---|
| bench's analytic shift (DATAC16, clean) | 10/10 | 10/10 | 10/10 | 10/10 | 7/10 acq, 0/10 del | 3/10 acq, 0/10 del |
| channel model's rotation (DATAC16, clean) | 10/10 | 10/10 | 10/10 | 10/10 | 7/10 acq, 0/10 del | 3/10 acq, 0/10 del |

That is the check that lets a session-level number be read as a mode-level
number, and it is why the session case can be run over the model.

## What we could not do here, and why it matters

The **session-level** case was not run in this study, for a reason worth
recording: the `-x fifo` backend is **not a real-time transport**.  A FIFO
carries samples as fast as the writer can put them there, so a whole burst
reaches the reader in a few milliseconds, while the modems' turn timers
are seconds long.  The two never line up: a callee that answers a CALL
while the caller is still sending is a collision, and both receivers see
nothing.  `utils/carrier_offset_air.py` paces the delivery and models the
half-duplex channel, and with them the link-level exchange works -- the
callee hears the CALL and answers -- but a *session* needs the
deterministic sock or rtp transport, which is what the integration
harness uses.

So: the case is runnable, and it is one command on the team's bench
(the command above).  It was not run here, and the tables above are
mode-level.

## What the team has to decide

1. **Should the OFDM window be widened?**  It is ±55 Hz today, and the
   knob already exists: `freedv_set_tuning_range()` is in the tree
   (`freedv_api.c:1585`), sets the same `fmin`/`fmax` for burst-mode OFDM,
   and nothing calls it.  Widening it is one call in `fdv_configure()`.
   The price is measured (the previous record): a ±150 Hz span captures
   ±153 Hz for 2.5× the idle CPU on x86, with zero false syncs in 600 s of
   noise.
2. **What is the cost of a wider window in interference?**  A wider search
   captures more carriers, and an interferer inside the window is a false
   sync.  The simulator has no QRM model, so this cannot be answered here
   -- the repo says so in three places (`mercury.ini.example`,
   `docs/RELEASE-1.9.22.md`, `docs/CHANNEL-MODEL.md`), and it is the
   obvious next measurement.
3. **Should the modem say anything when it cannot capture the carrier?**
   A station 140 Hz off gets "no sync" and nothing in the logs points at
   frequency.  A carrier estimate is already available
   (`freedv_get_foff_est`), so a log line naming the offset and the
   window would turn an invisible failure into a diagnosis.  The offset
   study's own correction is the argument for it: we published a wrong
   answer about this for a release because nothing in the modem said what
   the offset was.

## Caveats

- **Simulation only.** AWGN, the project's bench, no radio.  The capture
  window is a property of the receiver's search, so it should carry over;
  the *delivery* numbers at the fringe are a different question and belong
  to the OTA bench.
- **The window is an SNR-dependent property.** The tables here are at each
  mode's own operating point; at a healthy SNR the window is the same
  ±55 Hz but everything inside it is delivered.
- **DATAC3 measured 0/10 at −3 dB in the first study's bench** and was
  written off as a mode-specific floor.  It is the floor: DATAC3's knee is
  at −4.8 dB, so −3 dB is *above* it and the mode acquires 10/10 there.
  The first study's number was measured at −12 dB, below the floor, and
  should be read as "DATAC3 does not acquire at −12 dB", not as a defect.
- **The MFSK window was measured through the MFSK bench** (the real MFSK
  backend, its own search), not through the OFDM bench, because MFSK has
  its own backend (`modem/modem.c:606`): the OFDM sweep cannot drive it.
- **The per-mode windows are measured with 10-20 trials a point**, so the
  rate at the edge carries a few points of sampling noise.

## Reproducing

```sh
make -C modem/freedv && make -C utils && make -C tests test_watterson
./tests/test_watterson                                  # the model's own checks
utils/shift_selftest                                   # the instrument's check

# the calibration: the published DATAC16 table, at the threshold it was
# recorded at
utils/acquire_vs_decode DATAC16 100 -13 -8 awgn 0.45

# a mode's operating point (50% acquisition)
utils/acquire_vs_decode DATAC16 50 -14 -9 awgn

# one mode's capture window, at its operating point
utils/offset_acq_sweep DATAC16 20 -12.2 -80 80 10

# MFSK's window, through the MFSK backend
FOFF=100 utils/mfsk_channel_sweep.sh awgn 100 10 -3

# the channel model's own offset, and its equivalence with the bench's shift
utils/acquire_vs_decode DATAC16 20 -100 -100 awgnc 0.4 50
MERCURY_CH_ENGINE=watterson MERCURY_CH_FREQ=50 \
    go test -count=1 -run 'TestMercuryARQTransfer$' -v .   # (in tests/integration)
```

## See also

- `docs/OFFSET-ACQUISITION-FINDINGS.md` -- the DATAC16 window, the
  instrument's correction, and what a wider search costs.
- `docs/MFSK-PORT.md` -- MFSK's own acquisition search and its ±100 Hz
  tolerance.
- `docs/CHANNEL-MODEL.md` -- the channel model, the carrier offset, and
  its follow-up list.
- `docs/ACQ-TUNING-FINDINGS.md` -- the threshold study, whose instrument
  was the one that had to be fixed.
