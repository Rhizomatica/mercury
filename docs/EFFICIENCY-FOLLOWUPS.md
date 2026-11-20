# Throughput next: where the protocol can still gain

Reads [EFFICIENCY-FINDINGS.md](EFFICIENCY-FINDINGS.md) and asks the question
its numbers leave open: which of the losses it measured can a protocol change
buy back, and which are waveform ceilings no protocol change touches. Desk
analysis, 2026-11-14, on `mercuryv2` at `274bd5c9`; the candidate-A grid
measured 2026-11-15 against the same tree. Every claim below cites a row in
the efficiency tables, a findings doc, or a function of `carousel.c`.
**Since `274bd5c9`, `mercuryv2` has moved the rungs this doc reasons about:**
#575 (clipped QAM16C2, candidate C, merged), #599 (DATAC17 and QAM16C2 on a
faster geometry -- 5.90 s / 1184 B and 3.30 s / 1183 B in `arq_mode_table`
-- and a ladder per bandwidth, BW2750's topped by QAM16C2W) and #602 (rung 1
of BW2300 and BW2750 is a 2 kHz MFSK; DATAC15 stays rung 1 of BW500 only).
The measurements here and in FINDINGS are of the earlier build; where a
figure moved, the text says so.

## The residue, decomposed

The 32 KB transfer at 25 dB AWGN measures 1954 bps. QAM16C2's raw piece rate
was about 2.6 kbps on the build that run measured (`arq_mode_table`: 1213 B
payload in 3.70 s, minus frame and segment headers), so the session achieved
**~75 % of the top rung** then. On current `mercuryv2` QAM16C2's slot is
1183 B in 3.30 s, about 2.87 kbps, and 1954 bps is **~68 %** of it (the
shape of the decomposition below does not change). (A frame-time note:
FINDINGS §3 lists the bare freedv bursts, measured at `237481e7` -- QAM16C2
1215 B in 3.30 s, DATAC16 16 B in 3.30 s -- while this doc quotes
`arq_mode_table`'s session slot, which is the burst plus a ~0.45 s margin,
with the payload minus headers. Both differ on purpose, DATAC16's slot at 14
B in 3.74 s and QAM16C2's at 1183 B in 3.30 s. #599 shortened QAM16C2's
burst to about 2.85 s (36 carriers, 1185 B), so today's 3.30 s slot shares
its number with §3's old burst by coincidence, not because the frame time
held.) The missing quarter -- about a third against today's table -- is, in
the order the efficiency report priced it:

- **the connect** (7.8–7.9 s of DATAC16 in every run, §1);
- **the climb** — the receiver starts every session cold, its per-level
  goodput table empty (`carousel.c`'s `rung[lv].sent/lost` die with the
  session), so early rounds run on low rungs until the probe economics
  (the link-adaptation comment above `PROBE_UP_DELIVERY` in `carousel.c`:
  "a probe costs one frame, a wrong climb costs a round") carry it up;
- **control frames between rounds** — the share the airtime table puts at
  6.7–24 % even for bulk;
- **the gaps the turns rules require** — Tg, D, and slot waits, the report's
  17–44 % silent share on message traffic (6–44 % with the bulk rows in);
  the hold-for-response part is #495's (§G).

And two ceilings protocol work cannot move:

- **the control waveform**: DATAC16 carries 14 B in 3.74 s, ~30 bps, and
  prices every CALL, ACCEPT, status and non-inline poll at one frame;
- **the top rung**: nothing above 16-QAM exists in the codec2 set. #599's
  BW2750 ladder tops out at QAM16C2W (1169 B in 2.85 s, about 3.3 kbps raw
  against QAM16C2's ~2.9) -- a wider geometry, not a higher order -- so the
  ceiling moved up on BW2750 but is still 16-QAM. That is why Mercury
  capped near 1950 bps on BW2300 at `237481e7` (before #599) where VARA
  reaches 2330–4990, pulling ahead from ~21 dB on AWGN (on the CCIR grids
  the two sit near 1 from 13 dB up; EFFICIENCY-FINDINGS §1).

## Candidates, ranked

| # | candidate | layer | expected gain | cost |
|---|---|---|---|---|
| A | per-peer rung memory | protocol, receiver-only, no wire change | measured below: -11 % clean, three of ten fade seeds to -17 %; +6 % cliff:8, +3..+18 % nvis — off by default | small: a state file + seeding the estimator |
| B | HARQ in the carousel's retx path | modem + protocol glue | large at low SNR and under fading | medium: hook exists, integration open |
| C | clipped QAM16C2 (2.0/1.0) | modem config, TX-only | fading crossover vs DATAC17 ~3 dB earlier | done: #575, merged 2026-11-17 (on air, 58 of 64 frames against none) |
| D | trim the connect handshake | protocol | ~4 s a session | small, touches the connect FSM |
| E | a short control burst | upstream codec2 | much of the 23–69 % control share on message traffic | large: a new waveform |
| F | rungs above 16-QAM | upstream codec2 | closes the VARA gap above ~21 dB (AWGN) | large: a new waveform; #599's QAM16C2W raised the 16-QAM ceiling on BW2750, not the order |
| G | the empty response slot after each keydown | protocol, turns timing | the largest single silence item on air: ~7 s per keydown (#495) | in flight: #603 (Pedro's `proto/early-slot-release` prototype) |

### A. Per-peer rung memory

Measured against trunk, 6 channels x 5 seeds, bulk one-way, warm = the same
seed run again after its cold run wrote the hint (the prototype branch,
`MERCURY_RUNG_HINTS=<file>`; cold cells fresh):

| channel | cold median s | warm median s | median of per-seed deltas | per-seed delta |
|---|---|---|---|---|
| awgn:0.02 | 52.1 | 46.3 | **-11.1 %** | -11 x5 |
| cliff:3 | 361.7 | 361.7 | 0 | 0 x5 |
| cliff:8 | 75.0 | 79.4 | **+5.9 %** | +6 x5 |
| fade:3:0.5 | 575.6 | 521.6 | 0 | -12, 0, +2, -17, 0 |
| fade:8:0.5 | 199.0 | 199.0 | 0 | -16, 0, 0, 0, -4 |
| nvis | 1482.8 | 1646.5 | **+3.2 %** | +3, +18, +3, +10, 0 |

The wins are where a rung clearly delivers and the cold session spends early
rounds failing on the one above it; the fade seeds split — three of ten win
by 12–17 %, one by 4 %, one loses 2 %. The costs are deterministic, and they
are the design's real edge: on cliff:8, where the cold session never leaves
DATAC17, the hint buys nothing but re-aligns the poll schedule (+6 %, all
five seeds); on nvis, where nothing delivers above ~0.7, the hint steers
early rounds into re-measuring mid rungs and every seed pays (all five
slower or even). Seeding only what delivered (a rung that lost half its
frames seeds nothing) was needed even to get that far: the first cut seeded
DATAC1's 89 % loss into the next session's probe and it probed that rung
four times. The one -55 % fade win of the first, cap-limited grid did not
reproduce on the re-run; the table is the bounded result. And the original
estimate — "a stale hint costs one probe frame" — was too kind: on a channel
that moved, the cost is a few rounds of re-measuring.

That is why the branch ships **off by default**: `MERCURY_RUNG_MEMORY=1`
turns it on. Where the hint has something solid to say it buys a tenth of
the session (awgn, all five seeds) and takes three of the ten fade seeds by
12–17 %, a fourth by 4 %; where the channel never lets a rung deliver it
costs 3–18 %.
The open question for the on-air A/B is whether the hint should carry a
"settled" mark only when the top rung delivered cleanly, so a struggling
channel writes no hint at all — that keeps the clean and fade wins and
costs cliff:8 and nvis nothing, at the price of one more piece of state.

The link adaptation is the best-measured part of the protocol — per-level
delivery estimated from frame counts, dead levels verdict-ed, probes priced
one frame (`level_rate`, `level_goodput` and the probe rules in
`carousel.c`) — and it is reset to zero by every session end. What it
learned about a peer on a channel that changes over hours, not minutes, is
thrown away. Persisting it (keyed by peer callsign, decayed, written at
teardown, read at session start) seeds the estimator so a session between
the same two stations on the same band starts where the last one topped out.

Two properties make this safe. The receiver chooses the rung in this
protocol (CAROUSEL-TURNS §4), so the hint never crosses the wire — a Mercury
with memory interoperates with one without, in both directions. And the
existing gates still apply: a hint above what the current SNR estimate
supports is filtered by `level_allowed()` before it can cost anything, and a
stale hint that does get through costs a few rounds of re-measuring (the
grid above), never a wrong verdict. #545 improved what a session learns from
data frames' readings; this is the same idea one session earlier.

### B. HARQ in the carousel's retransmission path

The Chase-combining hook is in the vendored OFDM path, off by default,
measured (HARQ-FINDINGS: ~3 dB per doubling in AWGN, more under fading,
where copies fade independently; roughly half the frames rescued at the
cliff where single-shot delivers none). What is missing is the ARQ half:
mark a retransmission as a retx of the same codeword, reset the soft buffer
on new payloads, hold one frame's LLRs. The carousel already knows exactly
this — a block's pieces are re-sent until received — so the map from
carousel retx to combining opportunity is one-to-one. This is the lever at
the low-SNR fringe, where the MFSK floor lives (and DATAC15 at BW500; since
#602 rung 1 of BW2300 and BW2750 is the 2 kHz MFSK, which has its own
combining question).

### C. Clipped QAM16C2

Done: #575 merged on 2026-11-17 (CLIPPED-QAM16C2-FINDINGS has the
simulation; on air, 58 of 64 frames delivered against none). TX-side only,
interoperates with stock, fading goodput crossover vs DATAC17 ~3 dB earlier.

### D. Trim the connect

CALL then ACCEPT is one full DATAC16 round trip, 7.8–7.9 s, and the 4 KB
rows pay it whole — that is why they run at 40–75 % of the 32 KB rows'
bps. Letting ACCEPT carry the callee's first poll/status starts data one
control frame earlier; capability bits (HARQ support, hint availability)
could ride CALL rather than cost a separate exchange. Small money, but it
is the fixed cost every session pays.

### E and F. The waveform ceilings

Both belong upstream. E, a robust short control burst (~0.5–1 s), is what
would actually reclaim the control tax on message traffic — the turns work
already took the cheap half of it (inline polls: two keydowns per
request/response exchange instead of four, 16–32 % faster in the sim; the
0.64 s pattern ACK replacing the 3.7 s DISCONNECT frame). F, anything above
16-QAM at this code rate, is the only route to VARA's 4990 bps at 25 dB.
What is already in is a wider geometry (#599: QAM16C2W on BW2750, ~3.3 kbps
raw); what is still missing is a higher modulation order.
Both are codec2-scale projects; raising them upstream, with the efficiency
tables as the case, is worth more than any local patch.

### G. The empty response slot

#495 measured it on air (N3JXB to K7EK, 2026-11-07, both stations' logs
aligned to a few milliseconds): after each keydown the caller waits out an
empty response slot, ~7 s above the floor, before keying host data — the
largest single piece of the report's 17–44 % silent share on message
traffic. It is already being worked: #603 takes the slot as over once the
peer's keydown, had it come, would have been sensed (the prototype is
Pedro's `proto/early-slot-release` branch). The prototype's own commit
flags the price: it gives up the turns spec's never-on-the-air-together
guarantee, so it is a design decision, not a drop-in (off by default,
`car_set_early_slot`). The complementary lever needs no such trade: the #495 logs carry per-station
key-to-key latency, and the guards are compile-time constants
(`arq_protocol.h`: 700 ms channel guard, 900 ms post-ACK guard) sized for
one radio's switching latency — a guard derived from the measured peer is
protocol-side, and the INI route is closed (`channel_guard_ms` sits in the
config removed-keys list), so no operator knobs return.

## Two more, outside this doc's layer

Two candidates the report's numbers point at that this doc does not
develop, because both live above the protocol layer it measures:

- **Payload compression at the byte pipe.** Only part of the traffic this
  network carries compresses at the pipe: plain text does (2–4x), but NNCP
  packets are end-to-end encrypted — NNCP compresses before it encrypts,
  when it does at all — and Winlink B2F messages are already
  LZHUF-compressed, so a second pass gains neither. What the pipe can buy
  is a plain-text multiplier only, not a 2x on all real message bytes, and
  VARA has no compression, so whatever the measured ratio turns out to be,
  it is a differentiator the bps tables cannot see. Sizing it — the ratio
  on captured session payloads — is the prerequisite for ranking this
  against the modem gains in the table. A first cut is up as #562 (#506's
  proposal on #410's CALL2 negotiation, every knob off by default): the
  offer rides the CALL minor field, and the open wire question is that one
  minor value cannot carry both the encryption and the compression offer —
  the capability vector needs the ACCEPT's zero-checked bytes 10–13 back
  by a rule change of its own. It is not costed here because it changes
  what the tables measure — bytes per message, not bps.
- **Session reuse across queued messages.** The 4 KB rows run at 40–75 %
  of the 32 KB rows' bps because connect and climb amortize badly over
  short payloads. But the application layer already reuses sessions where
  it can: uucico moves every queued job in one session, and a B2F exchange
  carries all of its proposals in one session — for those hosts the fixed
  cost is paid once per session, not once per message, and the candidate's
  gain shrinks to whatever is left between their sessions. The gain that
  remains is for hosts that open a session per message; which hosts do
  that, and how much of the real traffic they carry, is the sizing
  question, and it needs data before this can rank. Where it does apply,
  holding a session open compounds D and A directly. Fairness, the
  lost-peer watchdog and the turns model all have a say, so it stays
  unranked until someone designs it.

## What this does not say

- A's grid is measured (§A); D's gain is still an estimate from the
  residue decomposition, not a measurement.
- The residue split (climb vs control vs gaps) is not measured separately —
  the report prices the session whole. The A/B above prices the climb's
  share for candidate A only.
- B's numbers are the freedv-layer measurements under synthetic fading, not
  carousel-integrated ones; integration changes the retransmission pattern
  the combining sees.
- Everything here inherits EFFICIENCY-FINDINGS' caveats: fixed seeds,
  simulator airtime, chart-read VARA figures, no radio.

## Offer

Happy to take any of these as far as wanted: A is prototyped on the branch
`rung-hint` (#570: `datalink_arq/rung_hint.c`, half-weight seed at session
start, write at teardown, off by default — `MERCURY_RUNG_MEMORY=1` is the
switch, sim A/B via `MERCURY_RUNG_HINTS`); D is a connect-FSM change I can
measure the same way; B needs a design pass with Rafael and Pedro before
code; C is done (#575); G is #603 (from Pedro's
`proto/early-slot-release` prototype) — bench time or a review pass is on
offer there; E and F are letters to
write, and the efficiency tables are the case to attach.
