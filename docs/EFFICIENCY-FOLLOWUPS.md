# Throughput next: where the protocol can still gain

Reads [EFFICIENCY-FINDINGS.md](EFFICIENCY-FINDINGS.md) and asks the question
its numbers leave open: which of the losses it measured can a protocol change
buy back, and which are waveform ceilings no protocol change touches. Desk
analysis, 2026-11-14, on `mercuryv2` at `274bd5c9`; every claim below cites a
row in the efficiency tables, a findings doc, or a line of `carousel.c`. One
prototype (candidate A) exists on a side branch; its A/B numbers are listed
here once measured.

## The residue, decomposed

The 32 KB transfer at 25 dB AWGN measures 1954 bps. QAM16C2's raw piece rate
is about 2.6 kbps (`arq_mode_table`: 1213 B payload in 3.70 s, minus frame
and segment headers), so the session achieves **~75 % of the top rung**. The
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
  6–31 % silent share, of which the hold-for-response part is #495's.

And two ceilings protocol work cannot move:

- **the control waveform**: DATAC16 carries 14 B in 3.74 s, ~30 bps, and
  prices every CALL, ACCEPT, status and non-inline poll at one frame;
- **the top rung**: nothing above QAM16C2 exists in the codec2 set, which is
  why Mercury caps at ~1950 bps where VARA reaches 2330–4990 above ~13 dB
  (EFFICIENCY-FINDINGS §1).

## Candidates, ranked

| # | candidate | layer | expected gain | cost |
|---|---|---|---|---|
| A | per-peer rung memory | protocol, receiver-only, no wire change | the climb, ~10–25 s a session at high SNR | small: a state file + seeding the estimator |
| B | HARQ in the carousel's retx path | modem + protocol glue | large at low SNR and under fading | medium: hook exists, integration open |
| C | clipped QAM16C2 (2.0/1.0) | modem config, TX-only | fading crossover vs DATAC17 ~3 dB earlier | one line, held for the OTA bench (#464) |
| D | trim the connect handshake | protocol | ~4 s a session | small, touches the connect FSM |
| E | a short control burst | upstream codec2 | much of the 19–50 % control share on message traffic | large: a new waveform |
| F | rungs above QAM16C2 | upstream codec2 | closes the >13 dB VARA gap | large: a new waveform |

### A. Per-peer rung memory

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
a stale hint that does get through costs one probe frame — the economics
the estimator was designed around. #545 improved what a session learns from
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

## What this does not say

- The gains for A and D are estimates from the residue decomposition, not
  measurements; the prototype's A/B against trunk is the number that
  counts, and it is not run yet.
- The residue split (climb vs control vs gaps) is not measured separately —
  the report prices the session whole. An A/B with and without the hint
  isolates the climb's share directly.
- B's numbers are the freedv-layer measurements under synthetic fading, not
  carousel-integrated ones; integration changes the retransmission pattern
  the combining sees.
- Everything here inherits EFFICIENCY-FINDINGS' caveats: fixed seeds,
  simulator airtime, chart-read VARA figures, no radio.

## Offer

Happy to take any of these as far as wanted: A is prototyped (the branch
carries `datalink_arq/rung_hint.c`, seeding at session start, decayed write
at teardown, and the sim knob `CAR_RUNG_HINTS` for A/B); D is a connect-FSM
change I can measure the same way; B needs a design pass with Rafael and
Pedro before code; C is Rafael's OTA run, where help with the A/B protocol
or the bench scripts is on offer; E and F are letters to write, and the
efficiency tables are the case to attach.
