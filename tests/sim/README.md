# Two-FSM ARQ Simulation Harness

An in-process, deterministic, discrete-event simulator that drives two real
`arq_session_t` FSMs against each other through a lossy virtual channel.  It
exists to validate ARQ protocol changes across thousands of randomized
loss/SNR patterns before any over-the-air testing.

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
  byte-for-byte.  A retransmission-based ARQ must deliver every byte; loss
  only adds latency, not data corruption.

`sim_prop_both_idle_or_disconnected(s)`:
  Both sessions are in a stable resting state -- either disconnected/
  listening, or connected and waiting in IDLE_ISS or IDLE_IRS.  This confirms
  the protocol converged rather than stalling in a retry loop.

`sim_prop_mode_floor_reached(ep, floor_mode, within_cycles)`:
  The endpoint's `payload_mode` has dropped to `floor_mode`.  Used by the
  S1 fade-cliff regression test to verify that a sustained high-loss channel
  drives the mode ladder down to the robust floor (DATAC15).

## v1 simplifications

**Single-global context swap (the s_active trick):** The FSM callbacks
(`g_cbs`) and `arq_conn` are file-scope globals in `arq_fsm.c` / the
production binary with no per-call context parameter.  Before each
`arq_fsm_dispatch`, `sim_endpoint_set_active(ep)` swaps `s_active` and
updates `arq_conn.my_call_sign`.  The callbacks then read `s_active` to
operate on the correct per-endpoint buffers.  This works because the event
loop is single-threaded and fully serialised.  A proper fix is a
`void *ctx` parameter threaded through the callback table (planned for S4).

**Stop-and-wait (burst_frames = 1):** The current production code has
`burst_frames = 1` in the mode table.  The per-endpoint outbox holds exactly
one frame.  An `assert` in `cb_send_tx_frame` catches any future change that
produces multi-frame bursts before the harness is updated to handle them.

**Simplified collision model:** Both endpoints transmitting at the same
virtual instant is theoretically possible but practically never occurs in
stop-and-wait sessions.  The scheduler schedules delivery independently for
each direction; a future enhancement could check airtime overlap and erase
both frames (half-duplex collision).

**Fixed simulated SNR:** Each delivered frame carries `rx_snr = 12.0 dB`.
This is enough for the FSM to record `local_snr_x10` and enable mode
upgrading, but does not exercise SNR-driven adaptation paths.  A richer SNR
model (sampled from a distribution or derived from PER) is a future task.

## Fault-injection API (channel controls)

The channel starts at each `sim_channel_cfg_t`'s base per-frame erasure `per`.
These calls change conditions mid-run (call them *after* `make_connected` so
the handshake completes on a clean channel, then the fault hits the transfer):

| Call (`sim_core.h`) | Effect |
|---|---|
| `sim_set_per(s, per)` | flat per-frame erasure probability [0,1] |
| `sim_set_rx_snr(s, db)` | SNR stamped on delivered frames (drives OLLA feedback) |
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
runs an 8 KB transfer and prints delivered/total, integrity, final mode and
conn-state. It links against whatever tree's `arq_fsm.c` it is compiled in, so
comparing two builds means compiling it twice (against tree A's and tree B's
sources) and diffing the output across a seed × channel grid. Trust the
**aggregate** over many seeds and always the integrity flag, not per-seed
deltas — the channel PRNG is a single shared stream, so once two builds make
different decisions they consume it differently and see different erasure
realizations. Used for the S1 merge decision (`docs/S1-FADECLIFF-DECISION.md`).

`ab_bench <seed> <channel> bidir` queues 8 KB on BOTH stations over a half-duplex
medium and prints the virtual time until both transfers complete (`done_ms`),
the collision count, and `STALLED` when both FSMs go idle with data still
undelivered.  That is the turn-handover bench: it measured the ACK + first data
burst in one keydown (1-2.5 % faster on clean/lossy channels, same completions,
no corruption).  `SIM_CS=<acq_ms>` turns the sim's carrier sense on (without
it every listen-before-talk check in the FSM is inert), and `SIM_STATS=1` (both
benches) prints the frames and airtime each mode used.

## The S1 fade-cliff regression (fixed)

`test_sim_fade_cliff_downgrades` connects on a good band, then drops the
channel SNR below the cliff of every mode except the DATAC15 floor
(`sim_set_snr` enables the mode-aware cliff erasure model).  It asserts the
FSM descends to DATAC15 and still delivers every byte.

The S1 bug was an UNBOUNDED reverse-loss hold in `record_tx_outcome()`: when
the SNR of surviving frames still looked adequate, every retry was charged to
reverse-path ACK loss, freezing OLLA and `consecutive_retries` so no downgrade
path (not even the hard-loss floor) could fire — the transfer starved above
the cliff.  The fix bounds the hold (`ARQ_REVERSE_HOLD_MAX`), advances
`consecutive_retries` per ACK timeout, and lets a mode probe change mode with
an unACKed window (the MODE_ACK carries the peer's `rx_expected`, which
resolves the window; undelivered bytes are restaged and re-framed at the new
mode).  The fuzz loop PER ceiling was raised 0.25 → 0.40 accordingly.

### Debugging the FSM under the sim

Build the sim binary with `-DSIM_TRACE_LOGS` to route the FSM's `HLOG*`
output (normally swallowed by the test stub) to stderr, each line prefixed
with the virtual uptime — invaluable for following mode negotiation, restage,
and retry decisions.

## Future work

- **HARQ LLR keying validation:** inject per-frame LLR arrays and verify
  that the soft-combining path in a future HARQ extension delivers the right
  bits.
- **Multi-frame burst support:** when `burst_frames > 1` is enabled in the
  mode table, the outbox assert will fire.  Update `sim_endpoint.c` to
  handle a burst queue (a small ring of `sim_outframe_t`), and update the
  scheduler to enqueue all frames in the burst before deciding delivery.
- **Upgrade the context swap to a proper `void *ctx` callback parameter**
  upstream (`arq_fsm_callbacks_t`).  This also fixes the S4
  static-`pending_burst_frames` race and makes the FSM safe for multi-
  instance embedding (e.g. a relay node running two concurrent sessions).

## Prototype: erasure-coded carousel ARQ (`carousel_bench.c`)

`carousel_bench <seed> <channel> [bidir]` runs a different ARQ design on the
same channel model, airtime table and half-duplex medium as `ab_bench`, so the
numbers compare directly.  It is a protocol simulation, not Mercury code.

**Design** (chosen by measurement; each rejected alternative is noted in the
code where it was tried):

- Data is cut into blocks of up to 96 pieces of 24 bytes, Reed-Solomon erasure
  coded over GF(256) (`rs_erasure.c`, systematic Cauchy: the first K pieces are
  the data, any K of them decode).  24-byte pieces fit every mode, so a block
  never has to be re-encoded when the mode drops.
- A round is one keydown of back-to-back frames.  It carries pieces of up to 8
  open blocks at once (oldest first, each with a margin for the loss seen), so
  a fast mode fills its keydown instead of paying a feedback turnaround every
  two or three frames.
- After each round the receiver sends ONE feedback frame: the window base and
  what each block still needs (or "not seen"), the loss, "I have data", and the
  round it answers.  No sequence numbers and no per-frame ACKs.
- Link adaptation picks the level with the best measured goodput (raw rate x
  delivered fraction, from decayed frame counts); probes only rungs that could
  beat it; a level's round is capped at one frame more than it recently
  delivered; a mode losing 4 frames in a row is dead, and caps the modes above
  it unless they have delivered recently themselves; dead modes are re-probed
  after 60 s, doubling to 8 min.
- A sender starts on the rung the peer's reported SNR supports, mapped as trunk
  enters a mode from DATAC15 (threshold plus hysteresis).  It is only a start:
  the first round on it is a one-frame probe and measured goodput takes over.
- The turn: a sender keeps it for at most a 120 s quantum while the peer has
  data, then yields at a block boundary.  Open blocks are only suspended.

**Results**, both stations sending 8 KB, against trunk (e06e00e + #321) with the
sim's carrier sense on (`sim_set_carrier_sense(s, true, 400)`; without it every
listen-before-talk check in the FSM is inert).  Seeds 1-20 were used while
tuning; seeds 21-40 are held out:

| channel         | trunk 1-20 / 21-40      | carousel 1-20 / 21-40   |
|-----------------|-------------------------|-------------------------|
| clean           | 231 / 234 s             | 124 / 125 s             |
| 10 %            | 275 / 294 s             | 141 / 152 s             |
| 25 %            | 416 / 450 s             | 209 / 230 s             |
| cliff 3         | 1373 / 1380 s           | 1140 / 1189 s           |
| cliff 10        | 310 / 309 s             | 249 / 247 s             |
| NVIS            | 0/20 in 3 h (1 KB, then no progress) | 5100 / 5392 s |
| fade 3 dB, 0.5 Hz  | 0/20 (11 KB of 16)   | 1532 / 1464 s           |
| fade 8 dB, 0.5 Hz  | 728 / 718 s          | 575 / 590 s             |
| fade 8 dB, 1 Hz    | 750 / 721 s (18/20)  | 589 / 563 s             |
| fade 15 dB, 0.1 Hz | 261 / 274 s          | 220 / 213 s             |
| fade 15 dB, 1 Hz   | 243 / 241 s          | 206 / 228 s             |
| fade 25 dB, 1 Hz   | 179 / 182 s          | 105 / 109 s             |

Every carousel run completes (NVIS and fade 3 dB need more than the default
30-minute `LIMIT_MS`: build with `-DLIMIT_MS='(8ULL*3600*1000)'`), with no
collisions and no corruption; trunk has 35-164 collisions per 20 runs.

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
