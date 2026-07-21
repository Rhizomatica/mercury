# ARDOP: ideas worth borrowing

## What this is

ARDOP (Amateur Radio Digital Open Protocol, by Rick Muething KN6KB; the actively
maintained fork is pflarue's) is another HF ARQ data modem. This is an
engineering evaluation of which of its techniques are worth borrowing into
Mercury — **not** a spec-compliance or interop effort. Each idea below is graded
against what Mercury already does, so the verdicts assume Mercury's current
post-rewrite ARQ (the 5-state, delivery-driven stop-and-wait FSM with the
Welch-Costas MFSK pattern ACK).

Source examined: `hermes/ardop/ARDOPC/` (`BusyDetect.c`, `ARQ.c`, `ARDOPC.c`,
`FEC.c`, `SoundInput.c`) and the ARDOP 2 specification document.

## Summary

| Idea | Where in ARDOP | Mercury today | Verdict |
|------|----------------|---------------|---------|
| Listen-before-talk: gate CALL on busy detector | `BusyDetect.c` `BusyDetect3()` | Detector exists, **never gates TX** | **BORROW** (do first) |
| Periodic ID / presence beacon | `Encode4FSKIDFrame()` + `Send10MinID()` | No auto beacon, no periodic timer | **BORROW** (more work) |
| Auto leader/timing negotiation | `EncodeConACKwTiming()` / `CalculateOptimumLeader()` | Fixed guards (deliberately) | **DEFER** |
| Decode-quality metric in ACK + gearshift | `EncodeDATAACK/NAK`, `Gearshift_9()` | SNR-free delivery-driven ladder; bit-less ACK | **SKIP** |
| Session ID = CRC-8 of callsigns in frame type | `GenerateSessionID()` | Random session_id + CRC16 addressing | **SKIP** |
| Memory ARQ (frame combining) | *(none — full retransmit)* | HARQ Chase soft-combining | **SKIP** (we're ahead) |

---

## BORROW — Listen-before-talk (busy-gated CALL)

**Top recommendation: highest value, lowest risk — the detector already exists.**

**ARDOP.** `BusyDetect3()` (`BusyDetect.c:189`) is a dual-band FFT carrier
detector: it compares signal power in a narrow band (top ~8 bins) and a wide band
(top ~66% of the searched bins) against the baseline, time-averaged with a slow
EMA (α≈0.2), with bandwidth-dependent thresholds and hysteresis (roughly 3 hits
to assert busy, 3 clears plus a ~5 s hold to release). Crucially, per its own
inline comment, it is *"only called while searching for leader … once leader
detected, no longer called"* — i.e. ARDOP gates **connection initiation** on the
channel being clear, but once a session is live it stops gating (you own the
channel).

**Mercury today.** We already have an equivalent, arguably cleaner detector:
`channel_busy_update()` (`modem/channel_busy.c:47`) classifies the 300–2700 Hz
passband peak against an EMA-tracked noise floor with `threshold_db`,
`hysteresis_db`, `on_debounce_ms` and `hang_ms` (see `modem/channel_busy.h`). It
is latched into `modem_channel_busy()` (`modem/modem.h:75`) and reported to the
host as `BUSY ON/OFF` (`modem/modem.c:~1887`, `tnc_send_busy`). **The gap:** it is
purely informational. A grep of `datalink_arq/` finds zero consumers — the ARQ
FSM transmits CALLs unconditionally. So Mercury has ARDOP's *sensor* but not its
*reflex*.

**Recommended design (to implement in a later pass):**

- **Gate only connection initiation** — the outgoing CALL and the manual CQ.
  Never gate in-session DATA or ACKs (once connected we own the channel), exactly
  as ARDOP does. Gate points:
  - first CALL: `fsm_idle`/`ARQ_EV_APP_CONNECT` in `datalink_arq/arq_fsm.c`
    (the `send_call_accept(sess, false)` at case entry, ~L623);
  - each retry: `fsm_calling`/`ARQ_EV_TIMER_RETRY` (~L747);
  - CQ: the `ARQ_CMD_SEND_CQ` path in `datalink_arq/arq.c`.
- **Keep the FSM modem-agnostic.** Do *not* call `modem_channel_busy()` directly
  from `arq_fsm.c` — the FSM is unit-tested with FFF fakes and must not depend on
  the modem. Add a `bool (*channel_busy)(void)` callback to
  `arq_fsm_callbacks_t` (`arq_fsm.h`), wire it in `arq.c` to
  `modem_channel_busy()`, and have the test/sim fakes return `false`.
- **Busy policy: hold, retry with backoff, then report busy.** If the channel is
  busy at CALL time, stay pre-CALL and re-arm the retry deadline (bounded
  backoff) up to a total LBT wait budget. If it is still busy at the budget,
  notify the host (busy/abort) rather than transmitting. This mirrors ARDOP and
  VARA and is friendlier on a shared channel than failing instantly.
- **Config-gated.** New knob (e.g. `busy_gate_tx`), default *on* when the busy
  detector is enabled; document it beside the existing `busy_*` knobs in
  `docs/TNC.md`. Interaction is benign: a host (uucp/VarAC) may also gate on the
  `BUSY` report — TNC-level LBT is just a politeness backstop, and double-gating
  does no harm.

**Why it matters now.** Mercury increasingly shares channels with Winlink,
VarAC and BPQ stations (e.g. the 14.110 neighbourhood). Gating our own CALLs on a
busy channel is basic good-neighbour behaviour and costs us almost nothing to
add given the detector is already built and tuned.

---

## BORROW (more work) — periodic ID / presence beacon

**ARDOP.** `Encode4FSKIDFrame()` builds a compact, FEC-protected ID frame
(callsign + Maidenhead grid, frame type `0x30`), and `Send10MinID()` (`ARQ.c`)
emits it every 600 s of operation for regulatory identification and presence; an
optional CW ID is also supported.

**Mercury today.** There is no automatic beacon and, more fundamentally, **no
periodic-timer framework** in the ARQ layer. CQ is manual-only: a one-shot
`ARQ_CMD_SEND_CQ` (`datalink_arq/arq.c`) driven by the host `CQFRAME` command;
all ARQ timers are per-session (retry/ACK/backlog), none free-running.

**Recommended design (to implement in a later pass):**

- Add a free-running periodic timer to the ARQ event loop (this scaffolding is
  reusable for other future features). While **idle/listening and not in a
  session**, emit the existing CQ frame via `arq_protocol_build_cq()`
  (`datalink_arq/arq_protocol.c:552`) as the ID, at a configurable interval
  (default **off**; regulators that mandate ID typically want ≤10 min).
- Reuse the callsign arithmetic codec (already SSID-clean after the recent
  dialed-SSID fix), so the beacon shows the correct callsign/SSID.
- **Open question to settle first:** grid square. ARDOP's ID carries a grid;
  Mercury's CQ frame currently carries the callsign only. Decide whether to
  extend the CQ payload with a grid or beacon callsign-only.

This is more work than LBT (new timer + config + a small state guard so we never
beacon mid-session), but it is self-contained and does not touch the data plane.

---

## DEFER — auto leader/timing negotiation

**ARDOP.** The responder measures the received leader length and reports it back
in `EncodeConACKwTiming()` (`ARDOPC.c`), and `CalculateOptimumLeader()` (`ARQ.c`)
adapts future leader length from it — though this path is currently disabled in
ARDOP itself.

**Mercury.** This collides with a standing project rule: **never expose ARQ
guard intervals as tuning knobs**, because the real guard requirement is
dominated by unmeasured, backend-specific audio buffering (ALSA/PULSE/soundcard),
not by the air interface. Negotiating leader/guard timing without first
characterising that buffering would optimise the wrong variable. **Defer** to a
future measurement study (instrument actual TX→RX turnaround latency per backend
first); not an implementation candidate now.

---

## SKIP (with reasons)

- **Decode-quality metric in ACK + gearshift.** ARDOP measures a per-frame
  constellation-scatter quality (5-bit, ~38–100) in `EncodeDATAACK`/`NAK`
  (`ARDOPC.c:~1852`) and drives mode changes from it in `Gearshift_9()` (`ARQ.c`).
  This conflicts head-on with the ARQ we just shipped: Mercury's ladder is
  deliberately **SNR-free and delivery-driven**, and the 0.64 s MFSK pattern ACK
  carries **no payload bits** at all. Adding a quality metric would force a coded
  ACK, losing the pattern ACK's speed and deep-fade robustness — a regression, not
  a gain. Our equivalent adaptation (delivery-driven ladder + HARQ combining)
  already covers this.

- **Session ID as CRC-8 of the two callsigns, XORed into the frame-type byte.**
  `GenerateSessionID()` (`ARQ.c:482`) gives ARDOP implicit per-session error
  detection. Mercury already has a random `session_id` plus CRC16 destination
  addressing, so the benefit is marginal and would cost an on-air format change.

- **Memory ARQ.** Worth stating explicitly: ARDOP has **none** — a NAK triggers a
  full-frame retransmit. Mercury already does better with HARQ Chase
  soft-combining across retransmissions. Nothing to borrow; we are ahead.

---

## Recommended order if/when we build

1. **LBT (busy-gated CALL)** — small, safe, reuses the tuned detector; add the
   FSM callback + hold/backoff + config knob.
2. **Periodic ID beacon** — after the timer scaffolding is in; settle the grid
   question first.

Each would be its own branch/PR and OTA-gated (São Roque ↔ Belo Horizonte), like
the rest of the ARQ work.
