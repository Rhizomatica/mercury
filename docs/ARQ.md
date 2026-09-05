# ARQ Datalink — Architecture and Protocol Reference

This document covers the ARQ (Automatic Repeat Request) datalink layer introduced
in Mercury v2 (`datalink_arq/`).  It replaces the original monolithic `arq.c` with
a table-driven, two-level hierarchical FSM and a clean protocol codec.

---

## Table of Contents

1. [Overview](#overview)
2. [Module Map](#module-map)
3. [Wire Protocol v4](#wire-protocol-v4)
4. [Frame Types and Subtypes](#frame-types-and-subtypes)
5. [Mode Timing Table](#mode-timing-table)
6. [Two-Level FSM](#two-level-fsm)
   - [Level 1 — Connection FSM](#level-1--connection-fsm)
   - [Level 2 — Data-Flow Sub-FSM](#level-2--data-flow-sub-fsm)
7. [Events](#events)
8. [Session Roles: ISS and IRS](#session-roles-iss-and-irs)
9. [Turn Mechanism](#turn-mechanism)
10. [Mode Upgrade / Downgrade](#mode-upgrade--downgrade)
12. [Timing Instrumentation](#timing-instrumentation)
13. [Logging](#logging)
14. [Tuning Guide](#tuning-guide)
15. [Source Files](#source-files)

---

## Overview

Mercury ARQ provides a reliable, half-duplex point-to-point data channel over HF
radio using the FreeDV modem stack.

Key properties:
- **Half-duplex**, explicit turn-taking (ISS/IRS roles).
- **DATAC16** is the *control-only* mode: CALL, ACCEPT, ACK and DISCONNECT are
  always 14 bytes on DATAC16 (a Mercury custom rate-1/3 mode that replaced
  DATAC13 as the control mode).
- **Data frames** start in DATAC15 (30 bytes payload, Mercury custom rate-1/3 mode) and
  may upgrade to DATAC4 (54 bytes), DATAC3 (126 bytes) or DATAC1 (510 bytes) based on
  SNR and backlog.  See `docs/MODES.md` for the full mode table with measured
  AWGN/MPP performance.
- **VARA-compatible TCP TNC** interface: control on `base_port` (default 8300), data on
  `base_port+1` (8301). This interface is frozen and not modified by the ARQ rewrite.
- **Broadcast** runs in parallel on a separate TCP port (default 8100) and is
  completely independent of ARQ.

---

## Module Map

```
datalink_arq/
  arq.c           — Public API entry-point, event-loop thread, TCP bridge threads
  arq.h           — Public API (frozen; VARA-compatible)
  arq_fsm.c/h     — Two-level hierarchical FSM (state tables + transitions)
  arq_protocol.c/h— Wire codec: frame builders, parsers, mode timing table
  arq_timing.c/h  — Timing instrumentation and [TMG] log records
  arq_modem.c/h   — Action queue (FSM→modem), PTT event injection (modem→FSM)
  arq_channels.c/h— Channel bus between TCP layer and ARQ event loop
  arq_events.h    — (generated) event forward declarations
  fsm.c/h         — Legacy generic FSM (kept for link compatibility)
  arith.c         — Arithmetic codec for callsign compression
  old_arq/        — Old 4495-line monolith (reference only, not compiled)
```

---

## Wire Protocol v4

All ARQ frames begin with a **framer byte** managed by `modem/framer.c`:

```
Byte 0: [packet_type(3)] | [extension_field(5)]
         bits [7:5] = packet_type
         bits [4:0] = packet-type-specific extension field
```

Packet type values:

| Value | Name                  | Used for                            |
|-------|-----------------------|-------------------------------------|
| 0x0   | `ARQ_CONTROL`         | ACK, DISCONNECT                        |
| 0x1   | `ARQ_DATA`            | Data payload frames                 |
| 0x2   | `ARQ_CALL`            | CALL and ACCEPT compact frames      |
| 0x3   | `BROADCAST_CONTROL`   | Broadcast subsystem (unrelated)     |
| 0x4   | `BROADCAST_DATA`      | Broadcast subsystem (unrelated)     |
| 0x5   | `ARQ_CQ`              | Compact DATAC16 CQ metadata frame   |

### Standard 8-byte header (ARQ_CONTROL and ARQ_DATA)

```
Byte 0: framer byte  (packet_type | extension_field)
Byte 1: subtype      (arq_subtype_t)
Byte 2: flags        bit7=TURN_REQ  bit6=HAS_DATA  bit2=BURST_END (DATA)
                     bit1=CTRL_ACKSEQ (CONTROL)  bits5/4/3=LEN_HI/B9/B10 (DATA)
Byte 3: session_id   random byte chosen by caller at connect time
Byte 4: tx_seq       sender's frame sequence number
Byte 5: rx_ack_seq   last sequence number received from peer (implicit ACK)
Byte 6: snr_raw      local RX SNR as uint8 = round(snr_dB) + 128; 0=unknown
Byte 7: ack_delay    IRS→ISS delay from data_rx to ack_tx, in 10ms units; 0=unknown
```

For **DATA frames** (`ARQ_DATA`), payload bytes follow immediately after byte 7.
The payload size is determined by the FreeDV mode in use.
For current `ARQ_CONTROL` and `ARQ_DATA` frames, the extension field is transmitted
as `0` and reserved for future use.

### CALL/ACCEPT compact frame (ARQ_CALL, 14 bytes)

CALL and ACCEPT use a different layout to fit two callsigns in 14 bytes:

```
Byte 0:      framer byte  (PACKET_TYPE_ARQ_CALL | BW token)
Byte 1:      connect_meta = (session_id & 0x7F) | (is_accept ? 0x80 : 0x00)
Bytes 2-3:   CRC16-CCITT of DST callsign (little-endian) — for local validation
Bytes 4-13:  arithmetic_encode(SRC callsign only) — 10 bytes, fits any realistic callsign
```

The transmitting side's callsign (SRC) is compressed with an arithmetic codec (`arith.c`).
The receiving side's callsign (DST) is not transmitted in full; instead its CRC16 is sent
so the receiver can silently discard frames not addressed to it.

BW token values in the framer-byte extension field:

| Token | Meaning   |
|-------|-----------|
| `0`   | Reserved  |
| `1`   | `BW500`   |
| `2`   | `BW2300`  |
| `3`   | `BW2750`  |

Handshake semantics:

- `CALL` advertises the caller's configured BW token.
- `ACCEPT` returns the negotiated session BW token, computed as `min(caller, callee)`.
- Once `ACCEPT` is processed, both peers know the session bandwidth without adding a
  separate negotiation round trip.

### CQ compact frame (ARQ_CQ, 14 bytes)

Mercury also defines a compact DATAC16 CQ metadata frame:

```
Byte 0:      framer byte  (PACKET_TYPE_ARQ_CQ | BW token)
Bytes 1-13:  arithmetic_encode(SRC callsign only)
```

The BW token uses the same values as `ARQ_CALL`. When a CQ frame is decoded,
Mercury emits `CQFRAME <source> <bw>` on the TNC control port.

### Integrity note

The framer byte no longer carries a CRC5. Mercury relies on the modem-layer
FreeDV frame integrity checks for whole-frame validation, while `CALL` / `ACCEPT`
still keep the destination callsign CRC16 so receivers can quickly reject connect
attempts not addressed to them.

---

## Frame Types and Subtypes

| Subtype value | Name            | Direction       | Mode used |
|---------------|-----------------|-----------------|-----------|
| 1             | CALL            | Caller → Callee | DATAC16   |
| 2             | ACCEPT          | Callee → Caller | DATAC16   |
| 3             | ACK             | IRS → ISS       | DATAC16   |
| 4             | DISCONNECT      | Either          | DATAC16   |
| 5             | DATA            | ISS → IRS       | DATAC15/4/3/1/17/QAM16C2|

Note: Subtype 12 (`FLOW_HINT`) from the old protocol was removed; the `HAS_DATA`
flag in byte 2 serves the same purpose.

---

## Mode Timing Table

Empirical values from NVIS HF path OTA testing.  All times are seconds.

| Mode    | Payload bytes | Frame duration | TX period | ACK timeout | Retry interval |
|---------|---------------|----------------|-----------|-------------|----------------|
| DATAC16 | 14            | 3.7 s          | 1.0 s     | 7.0 s       | 8.0 s          |
| DATAC15 | 30            | 4.4 s          | 1.0 s     | 11.0 s      | 12.0 s         |
| DATAC4  | 54            | 5.8 s          | 1.0 s     | 13.0 s      | 14.0 s         |
| DATAC3  | 126           | 3.8 s          | 1.0 s     | 11.0 s      | 12.0 s         |
| DATAC1  | 510           | 4.8 s          | 1.0 s     | 12.0 s      | 13.0 s         |
| DATAC17 | 1180          | 7.4 s          | 1.0 s     | 14.0 s      | 15.0 s         |
| QAM16C2 | 1213          | 3.7 s          | 1.0 s     | 11.0 s      | 12.0 s         |

- **ACK timeout**: measured from PTT-ON to ACK reception deadline.
  `= frame_duration + channel_guard + ACK_return_time`, rounded up.
- **Retry interval**: `= ack_timeout + ARQ_ACK_GUARD_S (1 s)`.
- **Channel guard**: 400 ms after PTT-OFF before next TX may start.

These values are defined as constants in `arq_protocol.h` and can be tuned there.

---

## FSM, Events, Turn Mechanism and Mode Upgrade

**RE-DO AFTER FSM REWRITE.** These sections described `MODE_REQ`/`MODE_ACK`,
the `MODE_REQ_WAIT`/`MODE_ACK_TX` states, `EV_RX_MODE_ACK`, the
`ARQ_BACKLOG_MIN_*` upgrade thresholds and keepalive — none of which exist any
more (verified by grep: zero references outside this file). The data plane is
now delivery-driven stop-and-wait with a pattern ACK, and the ladder is inferred
from what decodes rather than negotiated. Removed rather than left to mislead;
see `datalink_arq/arq_fsm.{c,h}` until rewritten.

## Session Roles: ISS and IRS

- **ISS** (Information Sending Station): the side currently transmitting data frames.
- **IRS** (Information Receiving Station): the side currently receiving and ACKing.

At connection establishment:
- **Caller** (side that issued `CONNECT`) → starts as **ISS** (sends data first).
- **Callee** (side in `LISTEN ON`) → starts as **IRS** (receives first).

This matches the VARA protocol convention and the UUCP use case where the
connecting side sends the session-initiation packets first.

---

### Delivery-driven downgrade (fade-cliff)

The primary rate controller is OLLA, a per-link SNR offset. But SNR alone
mispredicts on a fading or ISI-limited channel (e.g. NVIS), where a mode can
lose most of its frames while the *surviving* frames still measure a healthy
SNR. Three mechanisms keep the link from starving above such a cliff:

- **Bounded reverse-loss hold.** A retry with healthy peer SNR is normally
  attributed to reverse-path ACK loss (asymmetric HF) and the forward mode is
  held. This hold is capped at `ARQ_REVERSE_HOLD_MAX` (3) consecutive holds;
  a clean delivery re-arms it. Without the cap it froze OLLA and the retry
  counters, making every downgrade path unreachable.
- **Per-timeout failure accounting.** On a dead-enough channel no ACK ever
  arrives, so the normal outcome accounting never runs. Each ACK timeout now
  advances `consecutive_retries`, which feeds the hard-loss drop to the floor
  (`ARQ_HARD_LOSS_THRESHOLD`) without disturbing OLLA's tuned first-try-FER
  accounting.

## Timing Instrumentation

`arq_timing.c` records timestamps for every significant event in `arq_timing_ctx_t`
and emits `[TMG]` log lines.  These allow precise OTA round-trip analysis.

### Recorded events

| Function                           | Log tag     | What it records                          |
|------------------------------------|-------------|------------------------------------------|
| `arq_timing_record_tx_queue`       | `tx_queue`  | Frame submitted to action queue          |
| `arq_timing_record_tx_start`       | `tx_start`  | PTT ON (frame on air)                    |
| `arq_timing_record_tx_end`         | `tx_end`    | PTT OFF + on-air duration                |
| `arq_timing_record_ack_rx`         | `ack_rx`    | ACK received + OTA RTT + peer SNR        |
| `arq_timing_record_data_rx`        | `data_rx`   | Data frame decoded (IRS side) + SNR      |
| `arq_timing_record_ack_tx`         | `ack_tx`    | ACK TX started (IRS side) + local delay  |
| `arq_timing_record_retry`          | `retry`     | Retry event + attempt number + reason    |
| `arq_timing_record_turn`           | `turn`      | Role change + reason                     |
| `arq_timing_record_connect`        | `connect`   | Session established                      |
| `arq_timing_record_disconnect`     | `disconnect`| Session ended + session totals           |

### OTA RTT computation

```
OTA_RTT = (ack_rx_ms - tx_start_ms) - ack_delay × 10 ms
```

`ack_delay` is the IRS-reported time between receiving the data frame and
starting the ACK transmission (byte 7 of the control frame header).

---

## Logging

Mercury uses an asynchronous ring-buffer logger (`common/hermes_log.c`).

### Log levels

| Level   | Tag   | When emitted                                       |
|---------|-------|----------------------------------------------------|
| DEBUG   | `DBG` | Internal FSM traces (`-v` verbose mode only)       |
| TIMING  | `TMG` | Protocol timing events (always when file/JSONL used)|
| INFO    | `INF` | General status (default level)                     |
| WARN    | `WRN` | Retries, degraded conditions                       |
| ERROR   | `ERR` | Fatal / unexpected errors                          |

### CLI flags

| Flag        | Effect                                                         |
|-------------|----------------------------------------------------------------|
| `-v`        | Set console log level to DEBUG (shows all `[DBG]` and `[TMG]` lines) |
| `-L <path>` | Write all log levels (DEBUG+) to file at `<path>`             |
| `-J`        | Combine with `-L`: write file in **JSONL** format for machine parsing |

Example for OTA analysis:

```sh
./mercury -v -L /tmp/mercury-session.log
```

JSONL example (one JSON object per line in the log file):

```json
{"ts":"14:42:36.900","uptime_ms":12224,"level":"TMG","component":"arq","msg":"tx_start seq=0 mode=18 backlog=54"}
{"ts":"14:42:39.838","uptime_ms":15162,"level":"TMG","component":"arq","msg":"tx_end seq=0 dur_ms=2938"}
{"ts":"14:42:42.110","uptime_ms":17434,"level":"TMG","component":"arq","msg":"ack_rx seq=0 rtt_ms=2272 ack_delay_ms=410 peer_snr=2.2dB"}
```

Parse with `jq`:

```sh
jq 'select(.level == "TMG")' /tmp/mercury-session.log
```

---

## Tuning Guide

Use the `[TMG]` timing lines from a real OTA session to verify and adjust
the mode timing table in `arq_protocol.c`.

### Key measurements to check

1. **`tx_end` duration** (`dur_ms`) — compare against `frame_duration_s` in the table.
   If actual TX is longer, increase `frame_duration_s` and `ack_timeout_s` accordingly.

2. **`ack_rx` RTT** — should be less than `ack_timeout_s × 1000` ms.
   If RTT regularly exceeds ack_timeout, retries will be spurious.
   Rule of thumb: `ack_timeout_s ≥ frame_duration_s + 0.4 + ack_return_duration`.

3. **Retry rate** — a `[WRN] Data retry` log with `left=N` where N is near max_retries
   (`ARQ_DATA_RETRY_SLOTS = 10`) indicates marginal timing.  Increase `ack_timeout_s`.

4. **Turn latency** — time between `tx_end` on ISS and first `tx_start` on new ISS.
   Should be < 2 × `ARQ_CHANNEL_GUARD_MS` (700 ms).

### Common issues

| Symptom                                 | Likely cause                              | Fix                                    |
|-----------------------------------------|-------------------------------------------|----------------------------------------|
| Spurious retries despite good SNR       | `ack_timeout_s` too short                 | Increase by 1–2 s in mode table        |
| Long gaps between turns                 | `peer_payload_hold_s` too large           | Lower `peer_payload_hold_s` (e.g. 8–10) in the `[arq]` INI section |

### Timing constants quick reference

All in `arq_protocol.h`:

```c
#define ARQ_CHANNEL_GUARD_MS          700   /* armed at frame DECODE (fires
                                             * ~200ms before sender PTT-OFF), so
                                             * the effective clear-air gap is
                                             * ~500ms; the radio needs ~340ms
                                             * for TX->RX switch */
#define ARQ_ACK_GUARD_S               1     /* slack added to retry interval    */
#define ARQ_CALL_RETRY_SLOTS          4     /* CALL retries                     */
#define ARQ_DATA_RETRY_SLOTS          10    /* DATA retries before disconnect   */
#define ARQ_DISCONNECT_RETRY_SLOTS    2
#define ARQ_PEER_PAYLOAD_HOLD_S       15    /* hold payload mode after activity */
#define ARQ_SNR_HYST_DB               1.0f
#define ARQ_HARD_LOSS_THRESHOLD       8     /* consecutive retries => drop to floor */
#define ARQ_REVERSE_HOLD_MAX          3     /* bounded reverse-loss holds        */
```

Several of these are runtime-overridable from the `[arq]` section of the INI
file (`mercury.ini`) rather than fixed at compile time — they are `_Atomic`
globals seeded from the `_DEFAULT` value above and set once at startup:
`channel_guard_ms`, `iss_post_ack_guard_ms`, `data_retry_slots`,
`mode_hold_after_downgrade_s`, `ladder_up_successes`, `retry_downgrade_threshold`,
`peer_payload_hold_s`
(each clamped to a safe range). See `mercury.ini.example`
for keys, defaults, and ranges.

### Diagnostic environment variables

Not operator settings — deliberately env-only, off by default, and not written
to the INI, because each one disables an adaptive behaviour and would be a
foot-gun as a config key. They exist so a single question can be asked of a
live link (including on air) without building a special binary.

| Variable | Effect |
|---|---|
| `MERCURY_HARQ=0` | Kill-switch for HARQ Chase soft-combining (`harq_enabled()`). Combining is otherwise on for data modes. |
| `MERCURY_PIN_LADDER=<n>` | Pin the payload ladder to rung `n` (`0..ARQ_LADDER_LEVELS-1`) so one mode is exercised on every burst, instead of the delivery-driven climb. Out-of-range values are ignored; when it takes effect it logs `TEST HOOK: ladder pinned to level <n>` at WARN so a pinned run can never be mistaken for a normal one. |

Rung numbering follows `arq_mode_ladder` (0 = MFSK floor, 1 = DATAC4,
2 = DATAC3, 3 = DATAC1, 4 = DATAC17, 5 = QAM16C2).

---

## Source Files

| File                         | Responsibility                                      |
|------------------------------|-----------------------------------------------------|
| `datalink_arq/arq.c`         | Public API, event-loop thread, TCP bridge threads   |
| `datalink_arq/arq.h`         | Public API (VARA-compatible, frozen)                |
| `datalink_arq/arq_fsm.c`     | FSM state handlers and transition logic             |
| `datalink_arq/arq_fsm.h`     | State/event enums, `arq_session_t`, FSM API         |
| `datalink_arq/arq_protocol.c`| Frame builders, parsers, mode timing table          |
| `datalink_arq/arq_protocol.h`| Wire format constants, codec API                    |
| `datalink_arq/arq_timing.c`  | Timing recorder; emits `[TMG]` log lines            |
| `datalink_arq/arq_timing.h`  | `arq_timing_ctx_t`, recorder API                    |
| `datalink_arq/arq_modem.c`   | Action queue (FSM→modem TX), PTT event injection    |
| `datalink_arq/arq_modem.h`   | Action queue and PTT API                            |
| `datalink_arq/arq_channels.c`| Channel bus between TCP layer and event loop        |
| `datalink_arq/arq_tnc.c`     | ARQ→TNC notification seam (registered callback table)|
| `datalink_arq/arq_tnc.h`     | `arq_tnc_callbacks_t` and seam invoker API          |
| `common/cfg_utils.c`         | INI config load/write, incl. `[arq]` runtime tunables|
| `common/hermes_log.c`        | Async ring-buffer logger with TIMING level and JSONL|
| `common/hermes_log.h`        | Logger API and `HLOGD/T/I/W/E` macros               |
| `modem/framer.c`             | 3-bit packet_type + 5-bit extension-field encoding  |
| `common/crc6.c`              | CRC helper implementations (legacy CRC5 still lives here) |
