# Two-FSM ARQ Simulation Harness

An in-process, deterministic, discrete-event simulator that drives two real
`arq_session_t` FSMs -- connect, the carousel data plane, teardown -- against
each other through a lossy virtual channel.  It exists to validate ARQ
protocol changes across thousands of randomized loss/SNR patterns before any
over-the-air testing.  `test_arq_sim` covers the lifecycle and fuzz;
`test_carousel_sim` the carousel inside the FSM; `test_carousel` the carousel
alone (`carousel_sim.c`).

## How to run

```
make -C tests test_arq_sim
./tests/test_arq_sim
```

The binary prints one line per test.  The full suite completes in a few
seconds on any development machine because all time is virtual -- no wall
clock, no `sleep`, no radio.

## Reproducing a failing fuzz seed

When `test_sim_fuzz` fails, the failure message includes the seed:

```
fuzz seed=17 per=0.183 guard=420ms xfer=1234: length mismatch: ...
```

To isolate seed 17, change the loop range in `test_sim_fuzz` to run only
that seed:

```c
for (int seed = 17; seed <= 17; seed++)
```

Rebuild and run.  The same PRNG derivation always produces the same channel
parameters, so the failure is fully reproducible.

Alternatively, build with `-DSIM_TRACE` and add trace prints inside
`sim_core.c` dispatch functions (gated behind that macro) to watch the
virtual clock, active endpoint, and event ID on every dispatch.

## What each property means

`sim_prop_integrity(src, dst, sent, sent_len)`:
  The dst endpoint received exactly `sent_len` bytes and they match `sent`
  byte-for-byte.  The ARQ must deliver every byte; loss only adds latency,
  not data corruption.

`sim_mode_frames(s, mode)` (`sim_core.h`) counts the carousel frames keyed in
each mode, which is how `test_car_bandwidth_caps_the_rungs` checks that a
BW500 session never keys a wideband rung.

## v1 simplifications

**Single-global context swap (the s_active trick):** The FSM callbacks
(`g_cbs`) and `arq_conn` are file-scope globals in `arq_fsm.c` / the
production binary with no per-call context parameter.  Before each
`arq_fsm_dispatch`, `sim_endpoint_set_active(ep)` swaps `s_active` and
updates `arq_conn.my_call_sign`.  The callbacks then read `s_active` to
operate on the correct per-endpoint buffers.  This works because the event
loop is single-threaded and fully serialised.  A proper fix is a
`void *ctx` parameter threaded through the callback table (planned for S4).

**Plain frames, one per keydown:** CALL, ACCEPT and DISCONNECT go through the
per-endpoint outbox, one frame at a time (an `assert` in `cb_send_tx_frame`
catches a second); the carousel's rounds go as keydowns of separate bursts
(`drain_keydown_from`), each burst its own channel draw.

**Collisions only on request:** two stations keyed at once destroy each
other's frames only with `sim_set_half_duplex(s, true)`, and a station hears
the peer's carrier only with `sim_set_carrier_sense`.  Anything about turns
needs both; they are off by default so older tests keep their semantics.

**SNR:** each delivered frame carries `rx_snr`, 12 dB unless `sim_set_snr`,
`sim_set_rx_snr` or `sim_set_fading` change it.  The FSM reads it on the
connect for the start rungs; the carousel's SNR gate reads it per frame.

## Fault-injection API (channel controls)

The channel starts at each `sim_channel_cfg_t`'s base per-frame erasure `per`.
These calls change conditions mid-run (call them *after* `make_connected` so
the handshake completes on a clean channel, then the fault hits the transfer):

| Call (`sim_core.h`) | Effect |
|---|---|
| `sim_set_per(s, per)` | flat per-frame erasure probability [0,1] |
| `sim_set_rx_snr(s, db)` | SNR stamped on delivered frames (the start rungs and the carousel's SNR gate) |
| `sim_set_snr(s, db)` | **coherent fade**: mode-aware SNR *cliff* erasure model + stamps the same SNR on survivors. A frame in a mode whose cliff (approx. `docs/MODES.md`) is above the channel SNR is erased ~90%; robust modes pass. This makes "downgrade" the winning move, as on real HF. |
| `sim_set_mode_per(s, tbl, n, db)` | **empirical per-mode erasure**: a `{freedv_mode, per}` table (e.g. measured on pathsim `--midlat-dist-nvis`) plus the delivered-frame SNR. Models ISI-limited channels where SNR reads healthy while fast modes fail. Overrides the cliff model. |
| `sim_set_fading(s, db, hz)` | **time-varying Rayleigh fading** around a mean SNR, independent per direction, at `hz` fade rate (sum of 8 sinusoids). Each frame's instantaneous SNR is sampled at 8 points over its airtime and combined by an exponential effective-SNR mapping (beta = the mode's cliff), then judged against the cliff: a frame survives a deep fade over about a quarter of its length. Flat fading only (no delay spread). Stamps the mean SNR on survivors. Overrides the other models. |

`test_sim_fuzz` sweeps flat AWGN erasure; `test_sim_fuzz_fading` sweeps the
cliff and per-mode-PER models (60 seeds), asserting the two invariants that
must hold on any channel — **no corruption** (delivered is a byte-exact prefix
of sent) and **clean termination** (completed or disconnected, never stuck
CONNECTED). `test_sim_peer_loss_disconnects` blacks out a peer mid-transfer and
requires a bounded disconnect.

## A/B throughput bench (`ab_bench.c`)

`ab_bench <seed> <channel>` (channel = `clean | awgn:<per> | cliff:<snr> | nvis |
fade:<snr>:<doppler_hz>`; built by `make -C tests ab_bench`)
runs an 8 KB transfer and prints delivered/total, integrity, the last
`payload_mode` and conn-state. It links against whatever tree's `arq_fsm.c` it is compiled in, so
comparing two builds means compiling it twice (against tree A's and tree B's
sources) and diffing the output across a seed × channel grid. Trust the
**aggregate** over many seeds and always the integrity flag, not per-seed
deltas — the channel PRNG is a single shared stream, so once two builds make
different decisions they consume it differently and see different erasure
realizations. Used for the S1 merge decision
([FADE-CLIFF-DECISION.md](../../docs/FADE-CLIFF-DECISION.md)), and since for
checking that a change leaves the carousel byte-identical.

`ab_bench <seed> <channel> bidir` queues 8 KB on BOTH stations over a half-duplex
medium and prints the virtual time until both transfers complete (`done_ms`),
the collision count, and `STALLED` when both FSMs go idle with data still
undelivered.  That is the turn-handover bench.  `SIM_CS=<acq_ms>` turns the sim's carrier sense on (without
it every listen-before-talk check in the FSM is inert), and `SIM_STATS=1` (both
benches) prints the frames and airtime each mode used.

## Idle sessions and the lost-peer watchdog (`idle_drop.c`, `floor_idle.c`)

An idle session must stay up on a lossy link: the lost-peer watchdog
(`ARQ_CAR_PEER_LOST_MS`, 240 s) ends a session when a station hears nothing
from its peer for that long.  Two benches measure it; neither is run by
`make test`.

- `idle_drop <snr_db> <per> <seeds> <idle_s> [fade_hz]` (`make -C tests
  idle_drop`) connects two real FSMs on the two-FSM sim, sets the frame loss
  once both ends are CONNECTED, and counts the runs that either end leaves.
  This sim has no pattern callback, so it does not model the floor's
  continuation slots.
- `floor_idle <from_seed> <to_seed> <channel>` (`make -C tests floor_idle`)
  runs the carousel alone, with patterns, and counts the runs in which a
  station went 240 s without hearing its peer (`silent_ms`): the runs the
  watchdog would end.  For an idle session use
  `CAR_CS_DECODABLE=1 CAR_SCRIPT='A30,B30+1800000'` and a lossy channel such
  as `cliff:-9:0.2`.

`test_car_idle_lossy_link_stays_connected` and
`test_carousel_idle_keeps_hearing_the_peer` are the tests for the same thing.

## The control mode's SNR (`CAR_CTL_SNR_OFFSET`)

`CAR_CTL_SNR_OFFSET=<dB>` lowers the **control mode's** SNR on the simulated
channel, and only its: the frames the carousel sends as polls, handovers and
status (the control mode is `ARQ_CONTROL_MODE` = `FREEDV_MODE_DATAC16`, the
only mode with the 14-byte control payload, so the channel tells them apart
by mode).  The default is 0, which is the channel as it reads.

The offset is the channel's, applied wherever the channel judges or reports a
frame's SNR -- the reading a modem would give, the cliff test that decides
the decode, the connect hint.  So it does two things at once, and they are
the same thing: where the control mode still clears its cliff (−7 dB in the
sim), the frames are decoded and delivered but **read low**, and everything
that reads them -- the smoothed estimate (`snr_ema = 0.7·ema + 0.3·reading`
with the two-reading snap at `SNR_GATE_DB`), the rung the ladder picks, the
control-mode deafness test -- follows them down; where the offset takes the
mode **below its cliff**, the frames stop decoding, like any mode below its
cliff, and the session falls to the floor.  That is one physical fact -- on
MPD the narrowband control frame sits in a notch the wide data modes average
across, reading ~10 dB below the wide modes at 25-35 dB, 1-4 dB below DATAC3,
and about 3-4 dB at +5 dB (#501) -- and the knob says it once.

**Which offset.** The gap is not one number, and it grows with SNR: ~10 dB
against the wide modes at 25-35 dB, where DATAC16 tops out near 2-3 dB while
they keep climbing; 1-4 dB against DATAC3 all along; about 3-4 dB at MPD's
+5 dB.  So about -10 stands for the high-SNR reading drag, and about -3 to -4
for a link like MPD near +5 dB.  The -10 floor run below is the control-deaf
regime -- the offset takes the mode under its cliff -- not what MPD does at
+3 dB.

```
$ CAR_CTL_SNR_OFFSET=-8 SIM_STATS=1 ./carousel_bench 4 fade:3:0.5
  mode 10:    1 frames    0 ok      5 s air      (DATAC1)
  mode 12:  103 frames   72 ok    393 s air      (DATAC3)
  mode 23:   25 frames   15 ok     94 s air      (DATAC16, the control mode)
seed=4 chan=fade:3:0.5 oneway a2b=8192 b2a=0 integrity=OK done_ms=756121
readings: ctl=-4.8 dB (15 frames) data=3.3 dB (77 frames)
```

The same seed without the knob rides DATAC3 the whole way (115 frames, 439 s
of air) and finishes in 575.6 s.  With the control mode 8 dB low the ladder
drops to DATAC3-only and -- on a fading channel, where the dips take the
control frames below the cliff -- 10 of the 25 control frames sent stop
decoding; the session takes 31 % longer.  That is the symptom #501 is about,
reproduced on demand: reading low where the mode survives, failing where it
does not.

**The reproduction Pedro's fix test needs** (control frames read low, the
session must still climb to DATAC17/QAM16C2) is message traffic on a clean
high-SNR channel -- bulk traffic has too few control frames for the drag to
bite:

```
$ SC='A2500+1000,B300+2000,A2000+4000,B300+6000,A2000+8000,B300+10000,A1000+12000,B300+14000'
$ CAR_LIMIT_S=3600 CAR_CTL_SNR_OFFSET=-10 SIM_STATS=1 CAR_SCRIPT="$SC" ./carousel_bench 1 cliff:16
  mode 10:    5 frames    5 ok     24 s air      (DATAC1)
  mode 12:    2 frames    2 ok      8 s air      (DATAC3)
  mode 24:    7 frames    7 ok     52 s air      (DATAC17)
seed=1 chan=cliff:16 oneway a2b=7500 b2a=1200 integrity=OK done_ms=229751
readings: ctl=6.0 dB (21 frames) data=16.0 dB (14 frames)
```

QAM16C2 never comes: at offset 0 the same script rides it (7 frames) and
finishes in 197.0 s, at -10 it is 229.8 s with message delays +23 % (mean)
and +49 % (p90), and the QAM16C2 rung is absent on every seed tried (1-3).

The `readings:` line is in `carousel_bench`'s output: the mean SNR the control
frames' estimates read and the mean the data frames' read, with their frame
counts.  `CAR_SNR_BIAS` is separate and older (the bench-measured -3.5 dB for
DATAC15, DATAC4 and DATAC16); the two add up.

**A/B, one-way 8 KB, 10 seeds** (`CAR_LIMIT_S=3600 ./carousel_bench <seed>
<channel>`), mean time to complete, every run finished:

| channel | offset 0 | offset -6 | offset -8 | offset -10 | data reading |
|---|---|---|---|---|---|
| `awgn:0.02` | 53.2 s | 86.8 s | 70.8 s | 70.8 s | 12.0 dB |
| `cliff:8` | 76.1 s | 102.6 s | 99.8 s | 115.4 s | 8.0 dB |
| `cliff:3` | 368.0 s | 399.8 s | 412.8 s | 412.8 s | 3.0 dB |
| `fade:8:0.5` | 242.4 s | 286.8 s | 290.3 s | 287.0 s | 8.1 dB |
| `fade:3:0.5` | 569.0 s | 691.5 s | 942.7 s | 1775.1 s | 3.3 dB |
| `nvis` | 1473.7 s | 1639.1 s | 1551.3 s | 1947.5 s | 10.0 dB |

(The control readings move by exactly the offset wherever the mode still
decodes; on the fading channels the dips take the mode below its cliff and
the delivered control frames' mean sits higher than the offset -- fewer
frames, the failed ones counted in the mode's SIM_STATS row instead.)

(The response is not monotonic: on `awgn:0.02`, -6 at 86.8 s is slower than
-8 at 70.8 s.  At -6 the gap sits just under the two-reading snap --
`SNR_GATE_DB` -- so the smoothed estimate stays between the control and the
data readings and QAM16C2 stays gated; at -8 the snap takes the estimate
back up to the data readings and the ladder reaches it.  The drag is worst
when the gap is just under the snap margin, not when it is largest, so an
A/B for #501's fix should sweep about -4 to -10, not take the single -10.)

Nothing was corrupted in any of the runs, and the offset 0 runs are the runs
without the knob, down to the frame count.  Where the mode stays above its
cliff the cost is the ladder's; where the fading takes it below (`fade:3:0.5`
at -8 and -10, whose mean sits on the cliff) the polls start failing too, and
the cost compounds: 65 % at -8, 3.1x at -10.

**Below the cliff: the floor.** On `cliff:0` with the control mode at -10 dB
-- ten below its cliff -- the polls mostly stop coming (27 keyed, 3 through)
and the session falls to the MFSK floor and crawls: 2395 s against 358 s at
offset 0, intact.  That is the control-deaf regime of #501 and #510, from the
same knob:

```
$ CAR_LIMIT_S=7200 CAR_CTL_SNR_OFFSET=-10 ./carousel_bench 1 cliff:0
seed=1 chan=cliff:0 oneway a2b=8192 b2a=0 integrity=OK done_ms=2395091
readings: ctl=-10.0 dB (3 frames) data=0.0 dB (128 frames)
```

Three limits: on the floor the control frames ride the floor mode (MFSK), so
the knob does not apply there; the decode failure is tied to the SNR -- it
happens when the offset takes the mode below its cliff, and not otherwise;
and the sim never runs the connect exchange, so a session whose control mode
is below its cliff (`cliff:0` at -10) still starts as if CALL and ACCEPT had
got through on DATAC16 -- only the connect hint reads low, by the offset.
A control mode that fails *without* its SNR being low (interference, a notch
the reading does not see) is a different model; that is the second knob in
the alternative proposal on #501.

`test_carousel_ctl_snr_offset_moves_only_the_control_reading` and
`test_carousel_ctl_snr_offset_below_the_cliff_falls_to_the_floor` are the
tests: the offset moves the control frames' reading and leaves the data
frames' alone while the session still completes (above the cliff), and takes
the session to the floor (below it); an offset of 0 is the run without the
knob.

## The fade-cliff regression

`test_sim_fade_cliff_completes` connects on a good band, then drops the
channel SNR below the cliff of every mode except the DATAC15/DATAC16 floor
(`sim_set_snr` enables the mode-aware cliff erasure model) mid-transfer, and
asserts every byte still arrives.  It was written for the old stop-and-wait
plane's S1 bug, an unbounded reverse-loss hold that froze its downgrade
paths so the transfer starved above the cliff
([FADE-CLIFF-DECISION.md](../../docs/FADE-CLIFF-DECISION.md)); it now holds
the carousel to the same standard.

### Debugging the FSM under the sim

Build the sim binary with `-DSIM_TRACE_LOGS` to route the FSM's `HLOG*`
output (normally swallowed by the test stub) to stderr, each line prefixed
with the virtual uptime -- for following the connect, the carousel's rung and
turn decisions (its `car_trace` lines) and the teardown.

## Future work

- **Upgrade the context swap to a proper `void *ctx` callback parameter**
  upstream (`arq_fsm_callbacks_t`).  This also fixes the S4
  static-`pending_burst_frames` race and makes the FSM safe for multi-
  instance embedding (e.g. a relay node running two concurrent sessions).

## The carousel's design record (`carousel_bench.c`)

`carousel_bench <seed> <channel> [bidir]` runs the carousel
(`datalink_arq/carousel.c`) alone on the same channel model, airtime table
and half-duplex medium as `ab_bench`, so the numbers compare directly.
`CAR_TRACE=1` prints every keydown.  It started as a protocol prototype;
what follows is the record of how its design was chosen, measured against the
stop-and-wait plane that was trunk then ("trunk" below).

**Design** (chosen by measurement; each rejected alternative is noted in the
code where it was tried):

- Data is cut into blocks of up to 96 pieces of 24 bytes, Reed-Solomon erasure
  coded over GF(256) (`rs_erasure.c`, systematic Cauchy: the first K pieces are
  the data, any K of them decode).  24-byte pieces fit every mode, so a block
  never has to be re-encoded when the mode drops.
- **The receiver drives.**  Mercury's modem runs a control decoder (DATAC16)
  and one payload decoder bound to the mode it expects; nothing on the air says
  which mode a burst is in.  So the receiver chooses the mode, and it is also
  the side that sees what arrives.  It sends a POLL in DATAC16: the window base,
  what each open block still needs, and "send me up to N frames in mode M".
- The sender answers with a round, one keydown of up to N bursts carrying
  pieces of up to 8 open blocks (oldest first, with a margin for the loss the
  poll reports), and keys only when polled.  Every frame says how many follow,
  so the receiver knows when the round ends and polls again.  No sequence
  numbers and no per-frame ACKs.
- Only the receiver keeps a round timer, so the sides cannot race.  If the
  sender is not on the air by the time it should be, the poll was lost: poll
  again at once, and do not score the mode for it (a second silent poll in a
  row is scored, so a mode too weak even to sync is still found dead).  If it
  is on the air but nothing decodes, poll when its carrier drops.
- Link adaptation, run by the receiver on what it received: the level with the
  best measured goodput (raw rate x delivered fraction, from decayed frame
  counts); probes only rungs that could beat it; a level's round is capped at
  one frame more than it recently delivered; a mode losing 4 frames in a row is
  dead, and caps the modes above it unless they have delivered recently
  themselves; dead modes are re-probed after 60 s, doubling to 8 min.
- A direction starts on the rung its measured SNR supports, mapped as trunk
  enters a mode from DATAC15 (threshold plus hysteresis).  It is only a start:
  the first round on it is a one-frame probe and measured goodput takes over.
  In Mercury the connect measures both directions, and the ACCEPT is the first
  poll.
- The turn: the receiver takes it with a HANDOVER poll that also announces its
  own first round, sent in the same keydown (the peer rebinds its payload
  decoder in the gap, as #310 does), after at most a 120 s quantum when both
  have data.  Open blocks are only suspended.

**Results**, both stations sending 8 KB, against trunk (e06e00e + #321) with the
sim's carrier sense on (`sim_set_carrier_sense(s, true, 400)`; without it every
listen-before-talk check in the FSM is inert).  Seeds 1-20 were used while
tuning; seeds 21-40 are held out.  "Omniscient" is the first prototype, whose
receiver decoded every mode and whose sender chose them:

| channel            | trunk 1-20 / 21-40    | omniscient      | receiver-driven |
|--------------------|-----------------------|-----------------|-----------------|
| clean              | 231 / 234 s           | 124 / 125 s     | 121 / 121 s     |
| 10 %               | 275 / 294 s           | 141 / 152 s     | 136 / 143 s     |
| 25 %               | 416 / 450 s           | 209 / 230 s     | 192 / 216 s     |
| cliff 3            | 1373 / 1380 s         | 1140 / 1189 s   | 1050 / 1040 s   |
| cliff 10           | 310 / 309 s           | 249 / 247 s     | 247 / 242 s     |
| NVIS               | 0/20 (1 KB, then no progress) | 5100 / 5392 s | 3854 / 3959 s |
| fade 3 dB, 0.5 Hz  | 0/20 (11 KB of 16)    | 1532 / 1464 s   | 1321 / 1287 s   |
| fade 8 dB, 0.5 Hz  | 728 / 718 s           | 575 / 590 s     | 482 / 543 s     |
| fade 8 dB, 1 Hz    | 750 / 721 s (18/20)   | 589 / 563 s     | 495 / 530 s     |
| fade 15 dB, 0.1 Hz | 261 / 274 s           | 220 / 213 s     | 215 / 206 s     |
| fade 15 dB, 1 Hz   | 243 / 241 s           | 206 / 228 s     | 205 / 217 s     |
| fade 25 dB, 1 Hz   | 179 / 182 s           | 105 / 109 s     | 102 / 107 s     |

Every carousel run completes (NVIS and fade 3 dB need more than the default
30-minute `LIMIT_MS`: build with `-DLIMIT_MS='(8ULL*3600*1000)'`), with no
collisions and no corruption; trunk has 35-164 collisions per 20 runs.
Rounds are sent as separate bursts back to back; a gap between them
(`-DBURST_GAP_MS`) of 300 ms costs 1-3 %, of 1 s 5-11 %.  The 30 s keydown cap
(`-DMAX_KEYDOWN_MS`) makes no difference between 15 and 60 s at 8 KB.

What decided it, in the order it was found:

- **Link adaptation**, not the coding: pinned to the best mode, the first
  version already won on the cliffs.  Goodput per level, estimated from counts
  (a quarter of one-frame probes vanish at 25 % flat loss), with dead-mode
  detection kept separate from selection -- one estimator could not both climb
  and avoid dead modes when raw rates span 60x.
- **Multi-block rounds** over per-block piece sizes: bigger pieces also cut the
  turnaround cost, but a block cut for a fast mode does not fit a slow one and
  had to be re-encoded on a drop, losing its progress (unfinished runs).
- **Turn quantum** over "hand over at every block boundary" (25 % loss 25 %
  slower) and over "finish the window first" (starved the peer on NVIS).
- **Evidence beats inference**: a false dead verdict on a lightly probed
  DATAC4 locked out a working DATAC3/DATAC1, and a re-probe backoff counted in
  rounds kept it in force for half an hour.
- **The SNR start hint**, found on the fading channel: trunk was 10 % faster
  at 15 dB because it goes straight to DATAC17 on the SNR it is told, while the
  carousel climbed from DATAC15 by probes (25-40 s of airtime per sender).  The
  hint gives the carousel the same information and is 30-45 % faster on the
  clean and flat-loss channels too; NVIS, where the 10 dB reading misleads,
  pays 1-5 % for it.
- **The receiver drives**, forced by the modem (one payload decoder, bound in
  advance).  At first a lost poll cost a whole round's window of silence (25 %
  loss: 258/390 s against the omniscient 209/230 s); re-polling when the sender
  is not on the air recovered it and more.  A window timer then collided with a
  repeated handover (NVIS, 2 collisions); polling on carrier drop removed that.
- **No per-piece index**: a block's pieces go out with consecutive indices, so
  a segment carries its first index and a count instead of one byte per piece.
  DATAC4 carries 2 pieces instead of 1, DATAC3 5 instead of 4; cliff 3, fade
  3 dB and NVIS got 8-13 % faster.
