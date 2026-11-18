# Throughput next: where the protocol can still gain

Reads [EFFICIENCY-FINDINGS.md](EFFICIENCY-FINDINGS.md) and asks the question
its numbers leave open: which of the losses it measured can a protocol change
buy back, and which are waveform ceilings no protocol change touches. Desk
analysis, 2026-11-14, on `mercuryv2` at `274bd5c9`; the candidate-A grid
measured 2026-11-15 against the same tree. Every claim below cites a row in
the efficiency tables, a findings doc, or a line of `carousel.c`. Candidate
A is prototyped and measured against trunk in the carousel sim (6 channels
x 5 seeds, bulk one-way, 1 h bench cap, every run intact; warm = the same
seed after a cold run wrote its hint):

## The residue, decomposed

The 32 KB transfer at 25 dB AWGN measures 1954 bps. QAM16C2's raw piece rate
is about 2.6 kbps (`arq_mode_table`: 1213 B payload in 3.70 s, minus frame
and segment headers), so the session achieves **~75 % of the top rung**.
(A frame-time note, since the two docs disagree on purpose: FINDINGS §3's
probe frames are the bare freedv burst -- 1215 B in 3.30 s, DATAC16 16 B in
3.30 s -- while this doc quotes `arq_mode_table`'s session slot: payload
minus headers, preamble and margin counted -- 1213 B in 3.70 s, DATAC16
14 B in 3.74 s.) The
missing quarter is, in the order the efficiency report priced it:

- **the connect** (7.8–7.9 s of DATAC16 in every run, §1);
- **the climb** — the receiver starts every session cold, its per-level
  goodput table empty (`carousel.c`'s `rung[lv].sent/lost` die with the
  session), so early rounds run on low rungs until the probe economics
  (carousel.c:919–925: "a probe costs one frame, a wrong climb costs a
  round") carry it up;
- **control frames between rounds** — the share the airtime table puts at
  6.7–24 % even for bulk;
- **the gaps the turns rules require** — Tg, D, and slot waits, the report's
  6–31 % silent share; the hold-for-response part is #495's (§G).

And two ceilings protocol work cannot move:

- **the control waveform**: DATAC16 carries 14 B in 3.74 s, ~30 bps, and
  prices every CALL, ACCEPT, status and non-inline poll at one frame;
- **the top rung**: nothing above QAM16C2 exists in the codec2 set, which is
  why Mercury caps at ~1950 bps where VARA reaches 2330–4990 above ~13 dB
  (EFFICIENCY-FINDINGS §1).

## Candidates, ranked

| # | candidate | layer | expected gain | cost |
|---|---|---|---|---|
| A | per-peer rung memory | protocol, receiver-only, no wire change | measured below: -11 % clean, three of ten fade seeds to -17 %; +6 % cliff:8, +3..+18 % nvis — off by default | small: a state file + seeding the estimator |
| B | HARQ in the carousel's retx path | modem + protocol glue | large at low SNR and under fading | medium: hook exists, integration open |
| C | clipped QAM16C2 (2.0/1.0) | modem config, TX-only | fading crossover vs DATAC17 ~3 dB earlier | one line, held for the OTA bench (#464) |
| D | trim the connect handshake | protocol | ~4 s a session | small, touches the connect FSM |
| E | a short control burst | upstream codec2 | much of the 19–50 % control share on message traffic | large: a new waveform |
| F | rungs above QAM16C2 | upstream codec2 | closes the >13 dB VARA gap | large: a new waveform |
| G | the empty response slot after each keydown | protocol, turns timing | the largest single silence item on air: ~7 s per keydown (#495) | in flight: Pedro's `proto/early-slot-release` branch |

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

The wins are where a rung clearly delivers and the cold session spends
early rounds failing on the one above it; the fade seeds split — three of
ten win by 12–17 %, one by 4 %, one loses 2 %. The costs are deterministic, and they
are the design's real edge: on cliff:8, where the cold session never
leaves DATAC17, the hint buys nothing but re-aligns the poll schedule
(+6 %, all five seeds); on nvis, where nothing delivers above ~0.7, the
hint steers early rounds into re-measuring mid rungs and every seed pays
(all five slower or even). Seeding only what delivered (a rung that lost
half its frames seeds nothing) was needed even to get that far: the first
cut seeded DATAC1's 89 % loss into the next session's probe and it probed
that rung four times. The one -55 % fade win of the first, cap-limited
grid did not reproduce on the re-run; the table is the bounded result.
And the original estimate — "a stale hint costs one probe frame" — was
too kind: on a channel that moved, the cost is a few rounds of
re-measuring.

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
one frame (carousel.c:916–955) — and it is reset to zero by every session
end. What it learned about a peer on a channel that changes over hours, not
minutes, is thrown away. Persisting it (keyed by peer callsign, decayed,
written at teardown, read at session start) seeds the estimator so a session
between the same two stations on the same band starts where the last one
topped out.

Two properties make this safe. The receiver chooses the rung in this
protocol (CAROUSEL-TURNS §4), so the hint never crosses the wire — a
Mercury with memory interoperates with one without, in both directions. And
the existing gates still apply: a hint above what the current SNR estimate
supports is filtered by `level_allowed()` before it can cost anything, and
a stale hint that does get through costs a few rounds of re-measuring
(the grid above), never a wrong verdict. #545 improved what a session learns from
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
the low-SNR fringe, where the MFSK floor and DATAC15 live.

### C. Clipped QAM16C2

Done and documented (CLIPPED-QAM16C2-FINDINGS): TX-side only, interoperates
with stock, fading goodput crossover vs DATAC17 ~3 dB earlier. Rafael has
taken the OTA A/B onto the sBitx pair (#464 thread).

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
Both are codec2-scale projects; raising them upstream, with the efficiency
tables as the case, is worth more than any local patch.

### G. The empty response slot

#495 measured it on air (N3JXB to K7EK, 2026-11-07, both stations' logs
aligned to a few milliseconds): after each keydown the caller waits out an
empty response slot, ~7 s above the floor, before keying host data — the
largest single piece of the report's 6–31 % silent share on message
traffic. It is already being worked: Pedro's `proto/early-slot-release`
branch takes the slot as over once the peer's keydown, had it come, would
have been sensed. The branch's own commit flags the price: it gives up the
turns spec's never-on-the-air-together guarantee, so it is a design
decision, not a drop-in (off by default, `car_set_early_slot`). The
complementary lever needs no such trade: the #495 logs carry per-station
key-to-key latency, and the guards are compile-time constants
(`arq_protocol.h`: 700 ms channel guard, 900 ms post-ACK guard) sized for
one radio's switching latency — a guard derived from the measured peer is
protocol-side, and the INI route is closed (`channel_guard_ms` sits in the
config removed-keys list), so no operator knobs return.

## Two more, outside this doc's layer

Two candidates the report's numbers point at that this doc does not
develop, because both live above the protocol layer it measures:

- **Payload compression at the byte pipe.** The traffic this network
  carries — NNCP packets, Winlink B2F, plain text — compresses 2–4x. No
  modem gain in the table is worth more than a 2x multiplier on real
  message bytes, and VARA has no compression: this is a differentiator the
  bps tables cannot see. A first cut is up as #562 (#506's proposal on
  #410's CALL2 negotiation, every knob off by default): the offer rides the
  CALL minor field, and the open wire question is that one minor value
  cannot carry both the encryption and the compression offer — the
  capability vector needs the ACCEPT's zero-checked bytes 10–13 back by a
  rule change of its own. It is not costed here because it changes what
  the tables measure — bytes per message, not bps.
- **Session reuse across queued messages.** The 4 KB rows run at 40–75 %
  of the 32 KB rows' bps because connect and climb amortize badly over
  short payloads, and real traffic is a queue of short messages, not one
  bulk transfer. Holding a session open across queued messages compounds
  D and A directly. Fairness, the lost-peer watchdog and the turns model
  all have a say, so it stays unranked until someone designs it.

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
code; C is Rafael's OTA run on the sBitx pair (#464), where help with the
A/B protocol or the bench scripts is on offer; G is already Pedro's branch
— bench time or a review pass is on offer there; E and F are letters to
write, and the efficiency tables are the case to attach.
