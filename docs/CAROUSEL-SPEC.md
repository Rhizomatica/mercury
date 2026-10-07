# Carousel data plane: protocol specification

Derived from the code by reading it, at commit 2c07bf4: every `file:line`
reference is to that commit.  Section 11 lists what has changed since (the
integrity work, branch carousel-integrity), and the rules it touched are
updated where they stand, marked **[since 2c07bf4]**.  The code is normative; this is the reference for
reviewing it, modelling it and re-implementing it.  What has been verified
against it is in [specs/carousel/README.md](../specs/carousel/README.md) (a TLA+
model of the block layer) and `tests/sim/car_explore.c` (the real carousel.c
under every loss and sensing-failure pattern up to a bound).  Section 8 lists
the risks found while writing it; section 9, where the older
[CAROUSEL-ARQ.md](CAROUSEL-ARQ.md) disagrees with the code.


Source of truth: `datalink_arq/carousel.c` (abbrev. **C**), `datalink_arq/carousel.h`
(**H**), `datalink_arq/arq_fsm.c` (**F**), plus the few runtime hooks they call
(`datalink_arq/arq.c` **A**, `modem/modem.c` **M**, `datalink_arq/arq_protocol.[ch]` **P**).
Every rule cites `file:line`. Where `docs/CAROUSEL-ARQ.md` disagrees with the code,
the code wins; the differences are listed in section 9.

Notation. `now` is the event time in ms. Timer `T := x` means `deadline[T] = x`
(`x == 0` is stored as 1, C:352). "Disarm T" means `deadline[T] = 0` (C:353).
`A(lv)` = frame airtime of rung `lv`; `gap(lv)` = inter-burst gap; `CTL` = air of
one control frame (see 1.4). `round_air(lv,n) = n*A(lv) + max(n-1,0)*gap(lv)` (C:194-197).

---

## 1. Scope and assumptions

### 1.1 What is modeled

The carousel runs only while the ARQ session is in `ARQ_CONN_CONNECTED` and
`sess->car_active` (F:2408). It replaces the stop-and-wait data-flow sub-FSM
(F:2591-2592). CALL/ACCEPT (connect) and DISCONNECT (teardown) are the generic
ARQ FSM; only the parts that touch the carousel are specified here (section 5.12).

### 1.2 Physical/link assumptions (what the code relies on)

| # | Assumption | Where it shows |
|---|---|---|
| A1 | Half duplex. A station never receives while its own keydown (or pattern) is on the air. `tx_busy` is true from `keydown()`/`send_pattern()` until `car_on_tx_done()` (C:382-387, C:1177, C:1644). | runtime emits exactly one `ARQ_EV_TX_COMPLETE` per keydown/pattern (A:170-175, F:2312-2314); a dropped keydown forces PTT-off so TX_COMPLETE still comes (A:250-266) |
| A2 | Two decoders during a session: the **control decoder** (DATAC16) always runs; the **payload decoder** is bound in advance to exactly one mode via `bind_rx` → `sess->peer_tx_mode` (F:1411, C:389-393). The MFSK call-listener is not fed while connected (M:2865-2873). | a burst in any other payload mode is not decoded at all, and is not even carrier (comments C:597-599, C:1217-1218) |
| A3 | Frames are **erasure-or-correct**: each coded frame carries CRC16 XOR a 16-bit session seed (M:1192); a frame is delivered to the carousel only if it matched the seed (`seeded`, M:2409, M:2041-2047 → A:775-789 → `ARQ_EV_RX_CAROUSEL`). **[since 2c07bf4]** Each block also ends in a 4-byte check (3.1), and a failed check ends the session (R1). | |
| A4 | Seed = CRC16(session_id ‖ nonce ‖ CALLER ‖ '/' ‖ CALLEE) (**[since 2c07bf4]** nonce: the callee's 16 bits from the ACCEPT, 3.4, little-endian), callsigns upper-cased, never 0 (0 → 0x5A5A) (F:1461-1480). Set on the callee when it sends a carousel ACCEPT (F:914-918), on the caller when it hears one (F:1758); cleared when the station enters DISCONNECTED/LISTENING (F:201-203, F:1515-1520). While set, HARQ is off and DATAC16/MFSK still accept plain-CRC frames (CALL, ACCEPT, DISCONNECT, broadcast) (M:1872-1897). | |
| A5 | **Patterns** (**[step 3]** ACK/BREAK tone lists bound to the session seed, `mfsk_session_patterns`; see section 12) (ACK/BREAK, ~0.64 s Welch-Costas tone bursts, `PATTERN_AIR_MS`, C:98) carry no bits beyond the 1-bit kind and **no session binding**. The detector runs only while `car_expect_pattern()` is true (C:1730-1733 → F:1384-1388 → A:1331 → M:2966-2978). Detection is reported ~0.25 s after the pattern ends (comment C:99-101). | |
| A6 | **Carrier sense** (**[step 3]** also a heard NAV header: `nav_busy_until_ms`, section 12) = `io.peer_keyed` = `peer_is_transmitting()` (F:1420, F:1245-1258): true iff (busy detector on and busy — off by default) OR `now < rx_frame_busy_until_ms` (a preamble was caught: busy for one frame duration of that decoder's mode, cleared when any frame decodes; A:1267-1283, M:2988-2995) OR `now - last_rx_sync_ms < ARQ_CHANNEL_SYNC_HOLD_MS (250)` (P:341, A:1295-1298). It cannot see a burst in a mode neither decoder is bound to, nor one too weak to sync on (hidden terminal). | |
| A7 | No reordering, no duplication on the channel; a frame is decoded at (approximately) its end. Event processing is serialized on one event-loop thread (`g_sess_lock`). | |
| A8 | Each carousel function is pure protocol: no clock, no threads; time arrives as `now` (H:25-27). The FSM converts `car_next_deadline()` into the session deadline (F:4010-4021) and fires `ARQ_EV_TIMER_CAROUSEL` → `car_on_time()` (F:2323-2334). | |

### 1.3 The ladder (rungs) and derived per-rung constants

`LADDER` (C:130-134); payload bytes and airtime from `arq_mode_table` (P:91-105,
read via `mode_payload`/`mode_air`, C:157-167). `ppf = (payload - 6) / 24`
(C:193). `gap` (C:136-139). `keydown_cap` (C:852-858). `block_k_for` (C:419-428).
`min_db` (C:776-787). Start threshold (C:1533-1551).

| lv | mode | payload B | A(lv) ms | ppf | gap ms | keydown_cap | block K | level_min_db | start if SNR ≥ |
|---|---|---|---|---|---|---|---|---|---|
| 0 | MFSK (floor) | 98 | 13500 | 3 | 200 | 2 | 15 | -99 | (default) |
| 1 | DATAC15 | 30 | 4400 | 1 | 100 | 3 (slow-rung cap) | 14 | -7 | -3 dB |
| 2 | DATAC4 | 54 | 5800 | 2 | 100 | 3 (slow-rung cap) | 22 | -6 | never a start |
| 3 | DATAC3 | 126 | 3820 | 5 | 100 | 7 | 80 | -1 | +1 dB |
| 4 | DATAC1 | 510 | 4810 | 21 | 100 | 6 | 96 | +3 | +8 dB |
| 5 | DATAC17 | 1180 | 7400 | 48 | 100 | 4 | 96 | +7 | +12 dB |
| 6 | QAM16C2 | 1213 | 3700 | 50 | 200 | 8 | 96 | +13 | +18 dB |

Control mode: DATAC16, 14 payload bytes, 3740 ms (P:92, P:208).

### 1.4 Global constants (C:51-124 unless noted)

| Name | Value | Name | Value |
|---|---|---|---|
| CAR_PIECE | 24 B (H:41) | CAR_MAX_K | 96 (H:42) |
| CAR_WIN | 8 (H:43) | CAR_NLEVELS | 7 (H:44) |
| CAR_POLL_BYTES | 14 (H:46) | CAR_KEYDOWN_MAX | 17 (H:47); runtime cap ARQ_KEYDOWN_FRAMES 17 (A.h:90) |
| FRAME_HDR / SEG_HDR | 2 / 4 B | FB_UNSEEN | 255 (wire 127) |
| RS_MAX_PIECES | 256 (rs_erasure.h:20) | SLOW_RUNG_FRAMES | 3 |
| TURN_QUANTUM_MS | 30000 | TURN_CAP_MS | 45000 |
| MAX_KEYDOWN_MS | 30000 | HEAD_MS (H) | 110 |
| TAIL_MS (T) | 200 | GUARD_MS (G) | 700 (P:211) |
| ISS_GUARD_MS (IG) | 900 (P:222) | CHAIN_GAP_MS (CG) | 300 (P:340) |
| RESEND_RUN | 2 | WINDOW_MARGIN_MS (WM) | 1000 |
| SENSE_MS (S) | 1400 | CARRIER_CHECK_MS (CC) | 250 |
| SENDER_SILENCE_MS (SS) | 90000 | FLOOR_POLL_EVERY | 4 |
| FLOOR_DEEP_DB | 3.0 | FLOOR_POLL_EVERY_DEEP | 12 |
| FLOOR_SILENT_MAX | 6 | FLOOR_LATE_MS | 9000 |
| PATTERN_AIR_MS | 640 | PATTERN_SENSE_EXTRA_MS | 2000 |
| FLOOR_ANSWER_MS | 4000 | FLOOR_SENSE_MS | 3000 |
| FLOOR_DELAY_MARGIN_MS | 2000 | HANDOVER_SOON_MS | 5000 |
| BLOCK_MIN_K / BLOCK_AIR_MS | 8 / 60000 (C:419-422) | CTL_FLOOR_MARK | 0x07 (C:184) |
| CTL_DEAF_ON_DB | -7.5 (C:185) | CTL_DEAF_OFF_DB | -6.0 (C:186) |
| ARQ_SNR_MIN_DATAC15_DB | -7.0 (P:369) | ARQ_SNR_HYST_DB | 5.0 (P:368) |
| ARQ_CAR_PEER_LOST_MS | 240000 (F:1391) | DISCONNECT drain default | 45 s (P:476) |

Link-adaptation constants (C:700-705, 796, 842-847): PROBE_UP_DELIVERY 0.5,
PROBE_EVERY 8, DEAD_PROBE_MS 60000, DEAD_PROBE_MAX_MS 480000, LV_DECAY 0.8,
DEAD_RUN 4, SNR_GATE_DB 3.0, ROUND_OVERHEAD_MS = 4400+G+IG+H+T = 6310,
FLOOR_OVERHEAD_MS = 640+G+IG+H+T = 2550.

Derived control airtimes:
- `ctl_on_floor(c) = io.pattern && peer_ctl_deaf` — **my** control frames go on MFSK (C:187).
- `peer_ctl_on_floor(c) = io.pattern && ctl_deaf` — the **peer's** control comes to me on MFSK (C:188).
- `peer_ctl_air(c) = peer_ctl_on_floor ? 13500 : 3740` (C:189-192).
- `peer_floor_wait_max(c)` (C:665-670) with `ctl = ctl_on_floor ? 13500 : 3740`:
  `G+H+ctl+T+WM + S+FLOOR_SENSE+H+ctl+T+FLOOR_ANSWER` = **18200 ms** (DATAC16) / **37720 ms** (MFSK).

---

## 2. Roles and per-station state (`car_t`, H:105-182)

Each station runs one `car_t`. Per direction, the **receiver drives** (polls) and
the **sender** answers. `sending == true` means "I hold the turn (I am the
sender of my direction)"; `sending == false && !idle` means "I am driving the
peer". Both ends start from `car_init` (C:1553-1561: everything 0/false except
listed).

### 2.1 Global / medium

| Field | Meaning | Init |
|---|---|---|
| `io` | callbacks (H:57-82) | copied |
| `sending` | I hold the turn | false; caller → true at start (C:1570) |
| `idle` | nothing in flight either way | false |
| `rx_level` | rung the payload decoder is bound to (mirror of last `bind_rx`) | 0 (but runtime binding set by ACCEPT path, see 5.12) |
| `snr_level` | start rung for the peer derived from SNR measured here (connect SNR) | `rx_level` arg |
| `snr_ema`, `snr_valid` | EMA (0.7/0.3) of decoder SNR over **every** decoded frame from the peer, control or payload (C:1607-1609) | 0 / false |
| `deadline[4]` | CAR_T_POLL, CAR_T_SEND, CAR_T_WAIT, CAR_T_SENSE; 0 = disarmed (H:86) | all 0 |
| `last_carrier_ms` | last time the peer was decoded, a pattern heard, or `peer_keyed()` returned true (C:363-368, C:1606, C:1683) | 0 |
| `tx_busy` | my keydown/pattern is on the air | false |
| `after_tx` | AFTER_NONE/POLL/ROUND/PATTERN: what `car_on_tx_done` arms (C:127) | NONE |
| `txbuf[17]` | frames of the current keydown | — |

### 2.2 Control-deafness (floor control plane)

| Field | Meaning | Init |
|---|---|---|
| `ctl_deaf` | I hear the peer below the control mode (hysteresis: on if `snr_ema < -7.5`, off if `snr_ema >= -6.0`, C:1610-1614); reported in every frame I send | seeded at connect (C:1594-1600) |
| `peer_ctl_deaf` | the peer reports it hears me below the control mode; overwritten by the bit in every decoded frame (C:1583-1590, 1617, 1621, 1625) | seeded at connect |

### 2.3 Sender side

| Field | Meaning | Init |
|---|---|---|
| `sb[8]`, `nsb` | open (unretired) blocks, oldest first; `car_sblock_t` (H:88-95): `id` (uint8), `K` (data pieces), `len` (bytes), `next` (next piece index to send, wraps mod 256), `need` (pieces receiver still needs, last report), `resend` (data piece the receiver's stream waits on, -1), `sent` (distinct pieces sent, capped at 256), `round_sent` (fresh pieces in my last round), `data[96][24]` | 0 |
| `next_blk_id` | uint8 id of the next block to open | 0 |
| `tx_level`, `tx_n` | rung / frame count the last poll directed (-1: never polled) | -1, 0 → set by start (C:1572) |
| `tx_poll_id` | 4-bit poll id my frames answer | 0 → 1 at start |
| `tx_loss` | receiver's reported loss (loss16/15) for the repair margin | 0.1 |
| `handover_unconfirmed` | my handover (or first) round is out, no poll/pattern yet | false |
| `floor_waiting` | a floor round of mine is out: pattern/poll/silence may come | false |
| `pat_waiting` | a round above the floor is out: an ACK pattern may come | false |
| `floor_silent` | floor rounds in a row with no answer | 0 |
| `floor_blk` | id of the block my last floor round carried | 0 |
| `floor_tx_end`, `floor_delay_ms` | end of my last floor round; learned answer delay (EMA 3/4) | 0 |
| `peer_snr_level` | start rung the peer measured for me (from every frame's snr_level field) | tx_level arg, or rx_level if -1 |
| `peer_has_data` | the peer's last poll/handover had has_data | false |
| `send_start_ms` | when my current sending turn began | 0 |
| `floor_yielded` | I held (did not continue) because the peer's handover is due | false |
| `floor_pats_heard` | patterns heard since the peer's last poll | 0 |
| `heard_poll_id` | id of the last poll I heard (echoed in my handovers) | 0 |

### 2.4 Receiver / driver side

| Field | Meaning | Init |
|---|---|---|
| `rb[8]`, `rbase` | receive window: slot `id % 8`, ids `rbase..rbase+7`; `car_rblock_t` (H:97-103): `known`, `done`, `K`, `len`, `have`, `delivered` (bytes already handed up), `got[256]`, `piece[256][24]` | 0 |
| `poll_id` | 4-bit id of my current poll (the round I expect) | 0 → 1 (callee) |
| `my_poll_id` | id of the last poll I actually sent with n>0 | 0 |
| `poll_level`, `poll_n` | rung/size I asked for (what I expect) | — |
| `drove_peer` | `poll_level` is a rung I have polled the peer on (never reset) | false |
| `round_seen`, `round_frames` | frames of the expected round decoded / announced total | 0 |
| `round_heard` | anything of the round heard (frame, status, or carrier during sense) | false |
| `status_seen` | STATUS answering my current poll id | false |
| `silent_polls` | 1 after one unscored re-poll, until something is heard | 0 |
| `peer_unopened` | the peer has data not yet cut into blocks | false |
| `peer_hi`, `peer_hi_known` | highest block id the peer has opened (resolved) | — |
| `drive_start` | when I started driving this turn | — |
| `done_in_round` | blocks decoded since the last poll/pattern | 0 |
| `rx_break` | a segment of a delivered/done block arrived, or a block completed, since my last poll/pattern | false |
| `loss_est` | EMA (0.5) of round loss | 0.1 |
| `polls` | counter of measured rounds | 0 |
| `floor_patterns` | patterns sent since my last poll | 0 |
| `polled_since_handover`, `polls_unheard` | a poll of mine is out since the peer's last handover; handovers in a row that did not echo my poll id | false, 0 |
| `pattern_for_lost_polls` | my last above-floor pattern stood in for unheard polls | false |
| `floor_streaming` | a floor round came since my last poll | false |
| `probe_off_floor`, `probe_lv`, `probe_n`, `probe_after_ms` | my last poll asked a floor-streaming sender for a higher rung | false |
| `floor_fallback` | that probe went unanswered: I listen at the floor | false |
| `floor_hold`, `floor_round_end` | empty floor window: waiting out the sender's own continuation | false, 0 |
| `peer_round_end`, `peer_round_lv`, `peer_round_n` | the peer's last round heard above the floor (for its 90 s repeat) | 0 |

### 2.5 Link adaptation (receiver measures the peer's direction)

Per rung `lv`: `lv_sent`, `lv_lost` (decayed frame counts), `lv_rounds`,
`lv_dead_run`, `lv_probe_fails`, `lv_round_at` (value of `polls` when last
measured), `lv_probe_at` (ms), `lv_backoff_ms` (H:139-143). All 0 at init.

---

## 3. Messages (wire formats)

All carousel frames ride the session-seeded CRC (A3). The modem appends CRC16
(2 bytes) to the mode's payload; the carousel sees exactly `payload` bytes
(M:1181-1194, M:2036-2037).

### 3.1 DATA (payload mode `LADDER[lv]`, length = full payload of that mode)

Encoder: `build_round` (C:479-586), `put_seg_hdr` (C:324-330). Decoder: `decode_data` (C:293-322).

| Byte/bits | Field | Semantics |
|---|---|---|
| b0[7:4] | `left` | frames after this one in the round (`nf-1-i`, C:581) |
| b0[3] | `unopened` | sender has app bytes not yet cut into blocks (C:407-410) |
| b0[2:0] | `snr_level` | start rung the sender's SNR gives the receiver's direction; value 7 → frame rejected (C:303) |
| b1[7:4] | `poll_id` | id of the poll this round answers (`tx_poll_id`, C:582) |
| b1[3:0] | `hi` | `(next_blk_id-1) mod 16` (C:582) |
| then ≤ 8 segments, each 4-byte header + pieces | | stop at count 0, at < 4 bytes left, or after 8 segments (C:306-320) |
| block bytes | | **[since 2c07bf4]** a block is the application's bytes followed by a 4-byte check, `len = app_len + 4`; K, pad and the pieces cover both. Check = CRC-32 (IEEE, reflected, init and final XOR 0xFFFFFFFF) over `seed` (4 bytes LE) ‖ `id` (the block's 8-bit id) ‖ `K` ‖ the application's bytes, stored LE. The receiver delivers only `app_len` bytes. |
| hdr[31:28] | block id mod 16 | |
| hdr[27:21] | K-1 | K ≤ 96 enforced on RX (C:316) |
| hdr[20:15] | count | pieces following (1..63); 0 terminates |
| hdr[14:7] | first | index of first piece; piece p has index `(first+p) mod 256` |
| hdr[6:2] | pad | `K*24 - len` (< 24 enforced, C:316) |
| hdr[1] | 0 | |
| hdr[0] | ctl_deaf | **first segment only** (byte 5 of the frame) = sender's `ctl_deaf` (C:583, C:304) |

Invariant: every DATA frame has ≥ 1 segment (C:550-574 always writes one; the
"top-up" break at C:556 requires `nseg > 0`). A frame failing a bounds check is
dropped whole (C:316).

Sent by: the sender (rounds), and by the receiver taking the turn (handover
round, after its HANDOVER frame in the same keydown).

### 3.2 Control: POLL / HANDOVER / STATUS (14 bytes)

Encoder `encode_ctl` (C:225-241); decoder `decode_ctl` (C:262-291).

| Byte | Bits | Field |
|---|---|---|
| b0 | [7:6] type (0 POLL, 1 HANDOVER, 2 STATUS; 3 → reject), [5] has_data, [4:1] poll_id, [0] ctl_deaf |
| b1 | [7:5] level, [4:1] n (frames asked for), [0] 0 |
| b2 | [7:4] loss16 = round(loss_est*15), [3:0] base = rbase mod 16 |
| b3..b9 | 8 × 7-bit `need[o]` for blocks base+o, first block in the high bits; 127 = not seen (FB_UNSEEN) |
| b10 | gap = first missing **data** piece of the base block; 255 = none |
| b11 | [7:5] snr_level, [4:2] h_level, [1:0] 0 |
| b12 | [7:4] h_n, [3:0] h_id |
| b13 | [7:4] hi, [3] unopened, [2:0] 0 |

Reject if `level`, `h_level` or `snr_level` ≥ 7 (C:289). Reserved bits are not checked.

Field use per type:

| Field | POLL (receiver → sender) | HANDOVER (receiver taking the turn) | STATUS (sender, polled, nothing to send) |
|---|---|---|---|
| poll_id | new id (or same id on a repeat) (C:1131-1132) | `heard_poll_id`: last poll **I heard** from the peer (C:1113) | the poll it answers (C:680) |
| level, n | rung and frame count asked; n=0 = "ack only, done" | 0 (unset) | 0 |
| base, need[], gap, loss16 | my receive window (`fill_poll`, C:1042-1062) | same | 0 (unset) |
| has_data | I have data to send (C:1059) | same (true) | 0 |
| snr_level | my `snr_level` | same | sender's `snr_level` |
| ctl_deaf | my `ctl_deaf` | same | same |
| h_level, h_n, h_id | 0 | rung, frames and poll id of my round that follows in this keydown (C:1108) | 0 |
| hi, unopened | 0 | my `next_blk_id-1`, `unopened()` (C:1109-1110) | `next_blk_id-1`, 0 (C:681) |

Carrier: `put_ctl` (C:245-260): if `ctl_on_floor(c)` (pattern-capable and the
peer reported `ctl_deaf`), the frame goes on **MFSK**, full 98-byte payload:
`b0 = CTL_FLOOR_MARK (0x07)`, `b1..b14 = encode_ctl`, rest 0. Otherwise DATAC16, 14 bytes.

RX demux (`car_on_frame`, C:1602-1626):
1. Always: `last_carrier_ms = now`; if `snr_db != 0` update `snr_ema`, `snr_valid`, `ctl_deaf` (C:1606-1615).
2. From the control decoder → `decode_ctl` → `note_peer_ctl_deaf(m.ctl_deaf)`; `on_ctl` (C:1616-1619).
3. Else if `len ≥ 15 && bytes[0] == 0x07` → control on the floor, same as 2 (C:1620-1623). (0x07 is unambiguous: a DATA b0 with snr_level 7 is invalid.)
4. Else `lv = level_of_mode(mode)`; if `lv ≥ 0` and `decode_data` ok → `note_peer_ctl_deaf`; `on_data(lv)` (C:1624-1625).

### 3.3 Patterns (no frame)

`io.pattern(kind)` → `ARQ_ACTION_TX_PATTERN` (F:1413-1418, A:232-241).
`CAR_PATTERN_ACK = 0`, `CAR_PATTERN_BREAK = 1` (H:84). RX: `arq_post_pattern_ack(is_break)` →
`ARQ_EV_RX_PATTERN` with `HAS_DATA` flag = BREAK (A:1148-1154) → `car_on_pattern` (F:2315-2319).

| Pattern | Sent by | Meaning |
|---|---|---|
| ACK (floor) | receiver, `poll_level == 0` | "keep going" (silence means the same at the floor, H:19-23) |
| BREAK (floor) | receiver | "the block your round carried is delivered" (`rx_break`). **[since 2c07bf4]** Advisory: the sender stops sending that block but holds it until a POLL retires it; a finished direction is acknowledged by an ack-only POLL at the floor too (T11). |
| ACK (above floor) | receiver, round came whole | "everything you sent of each block is in; the same again" (C:1347-1352, C:1689-1693) |
| BREAK (above floor) | receiver, round lost frames | **[carousel-nvis]** "the round lost no more than my last poll said (`loss16`); the same again". The sender credits each block with what it sent less that loss. Like the ACK it retires nothing: a wrong guess costs pieces until the next poll, never data. A round that lost more is polled. |

### 3.4 Connect frames (generic FSM, carousel-relevant fields only)

- CALL: carousel marker in the last byte of the SRC slot when the callsign code leaves it free (P:661-690, F:924-927). **[since 2c07bf4]** `0xA8`, **[step 3]** `0xA9`, **[carousel-nvis]** `0xAA` (inline polls, docs/CAROUSEL-TURNS.md §8); an earlier carousel's mark (`0xA7` 1.9.17/1.9.18, `0xA8`) reads as unmarked (that session runs stop-and-wait), and this one to it.
- **[since 2c07bf4]** Carousel ACCEPT: the callee's 16-bit session nonce in the last two bytes of its SRC slot (LE), when its callsign's code is at most 8 bytes; otherwise 0 on both ends (`arq_protocol_set_accept_nonce` / `_accept_nonce`). The same nonce on every ACCEPT of the session.
- ACCEPT (callee → caller) **is the first POLL**: names the caller's start rung in the top 3 bits of the DST-CRC field (P:624-641); value **7 = "level 0, and I hear you below the control mode"** (`ACCEPT_LEVEL_CTL_DEAF`, F:888-892). Carousel flag 0x10 in the framer extension (P:718). Plain CRC.
- DISCONNECT: plain CRC, session-id bound (7 bits, F:3929-3937); on MFSK to a peer the carousel last saw as control-deaf (`session_ctl_mode`, F:963-969).

---

## 4. Timers

Timer service: `car_on_time` fires, repeatedly, the armed timer with the
smallest deadline ≤ now (ties: lowest index POLL < SEND < WAIT < SENSE), clearing
it first (C:1740-1749). The FSM deadline is `min(car_next_deadline, car_last_rx_ms + 240000 if !idle, disconnect_deadline if pending)` (F:4010-4021).

`arm_poll(x)` ≡ `T_POLL := x + G` (C:1064).

### 4.1 CAR_T_POLL (receiver/driver: "the round is over or never came")

| Armed at | Value | Cite |
|---|---|---|
| `start_driving(…, lv, n, round_start, now)` | `round_start + H + round_air(lv,n) + T + WM + G` | C:1085 |
| `on_data` (each frame of the round) | `now + left*(A(lv)+gap(lv)) + T + G` | C:1461 |
| `on_ctl` STATUS (not sending) | `now + G` | C:1473 |
| `on_sense_timer`, carrier dropped after being seen | `now + G` | C:1399 |
| `car_on_tx_done` AFTER_POLL / AFTER_PATTERN | `floor_round_end + WM + (floor ? 9000 : 0) + G`, where `start = now - T + IG`, `floor_round_end = start + H + round_air(poll_level, poll_n) + T` | C:1657-1677 |
| `clear_of_peer_nudge` | `end + G` (end of the peer's predicted 90 s repeat) | C:1203 |
| floor fallback | `max(now + 13500 + 200, probe_after_ms + peer_floor_wait_max + H) + 3000` | C:1233-1241 |
| floor hold | `floor_round_end + peer_floor_wait_max + 3000` | C:1257-1260 |
| `defer_if_busy` | `now + 250` (busy) or `last_carrier_ms + 700` | C:372-380 |

Disarmed by: `send_handover` (C:1117), `send_poll_as` (C:1149), `send_pattern` (C:1176), `on_ctl` POLL (C:1494), `take_turn_if_idle` (C:1637).
Firing → `on_poll_timer` (5.6).

### 4.2 CAR_T_SEND (sender: key the round)

| Armed at | Value | Cite |
|---|---|---|
| `car_start_sender` (caller, on carousel ACCEPT) | `now` | C:1574 |
| `on_ctl` POLL with n>0 | `now + IG (900)` | C:1521 |
| `on_ctl` POLL with n=0 and own data | `now + 900` (with `tx_n = 1`) | C:1506-1507 |
| `car_on_pattern` accepted (floor or above) | `now + 900` | C:1703, C:1727 |
| `defer_if_busy` | as 4.1 | C:1757 |

Disarmed by `start_driving` (C:1084), `take_turn_if_idle` (C:1637).
Firing (C:1756-1760): if `defer_if_busy` → return; else `sending = true; send_round()`.

### 4.3 CAR_T_WAIT (sender: no poll came)

Armed by `arm_sender_wait(tx_end)` on every AFTER_ROUND `tx_done` (C:1651-1652), C:592-659:

```
answer = tx_end + G + H + peer_ctl_air + T + WM                         (5750 / 15510)
if peer_ctl_on_floor: bind_rx(0)                                         (C:596)
if drove_peer && poll_level == 0 && io.pattern:
    answer += S + FLOOR_SENSE + H + peer_ctl_air + T                     (C:605-606)
floor_waiting = (tx_level == 0 && io.pattern)                            (C:610)
pat_waiting   = (tx_level >  0 && io.pattern)                            (C:613)
if floor_waiting:
    ctl_answer = answer; answer += FLOOR_ANSWER (4000); floor_tx_end = tx_end
    if floor_delay_ms && tx_end + floor_delay_ms + 2000 < answer:
        answer = tx_end + floor_delay_ms + 2000                          (C:624-625)
    ctl_audible = snr_valid && snr_ema >= -7
    if ctl_audible && answer < ctl_answer: answer = ctl_answer           (C:631-632)
    if !ctl_audible && peer_ctl_on_floor && !peer_has_data
       && floor_pats_heard >= 4 && answer < ctl_answer: answer = ctl_answer   (C:643-645)
    if peer_has_data && tx_end + 5000 >= send_start_ms + 30000:
       answer = max(answer, tx_end + G + H + peer_ctl_air + T + 4000)    (C:652-655)
T_WAIT := (handover_unconfirmed || floor_waiting) ? answer : tx_end + 90000   (C:657-658)
```

Also armed: `take_turn_if_idle` → `now` (C:1638); yield → `now + 90000` (C:1782); floor silent max → `now + 90000` (C:1788); `defer_if_busy`.
Disarmed by: `start_driving` (C:1084), `on_ctl` POLL (C:1494), `car_on_pattern` accepted (C:1696, C:1714).
Firing → rule W (5.8).

### 4.4 CAR_T_SENSE (receiver: follow the answering carrier)

| Armed at | Value | Cite |
|---|---|---|
| `tx_done` AFTER_POLL | `start + S + (floor ? 3000 : 0)`, `start = now - T + IG` (i.e. tx_end + 2100 [+3000]) | C:1669-1675 |
| `tx_done` AFTER_PATTERN, above floor and `!pattern_for_lost_polls` | `start + S + 2000` (tx_end + 4100) | C:1657-1663 |
| `on_sense_timer` while peer keyed | `now + 250` | C:1396 |

Disarmed by `start_driving`, `send_handover`, `send_poll_as`, `send_pattern`, `on_ctl` POLL, `take_turn_if_idle`, floor fallback (C:1240).
Firing → `on_sense_timer` (5.7). It never keys; it can only pull T_POLL forward.

---

## 5. Transitions

Each rule: **guard → actions**. "Bind(lv)" = `bind_rx(lv)`: `rx_level = lv` and
the runtime rebinds the payload decoder to `LADDER[lv]` (C:389-393, F:1411).

### 5.1 Helper procedures

**P1 `has_data`** = `nsb > 0 || tx_pending() > 0` (C:402-405). **`unopened`** = `tx_pending() > 0` (C:407-410).

**P2 `open_block(max_k)`** (C:430-445): read ≤ `max_k*24` bytes from the app; if 0 → nothing. Else
`sb[nsb++] = {id = next_blk_id++, len, K = ceil(len/24), next 0, need K, resend -1, sent 0, data zero-padded}`.

**P3 `apply_need(m)`** (retire, C:457-475): for each open block b, `off = (b.id - m.base) & 15`;
`need = off ≥ 8 ? 0 : m.need[off] == UNSEEN ? b.K : m.need[off]`. need 0 → **retire** (drop, add len to
`retired`). Else `b.need = need`; `b.resend = (off == 0 && 0 ≤ m.gap < b.K) ? m.gap : -1`. Compact.
If `retired > 0` → `io.tx_confirmed(retired)` (F:1450-1456 updates BUFFER).

**P4 `build_round(lv, n, poll_id)`** (C:479-586), returns frame count nf (0 = nothing):
1. `margin = tx_loss*1.3 + 0.05`; `floor = (lv == 0 && io.pattern)`.
2. `queued = Σ ceil(need_b*(1+margin))`. While `queued < n*ppf(lv) && nsb < 8 && unopened && !(floor && nsb > 0) && (nsb == 0 || (uint8)(next_blk_id - sb[0].id) < 8)`: `open_block(block_k_for(lv))`, add its want (C:498-503).
3. If `nsb == 0 || n < 1` → return 0.
4. `round_sent = 0` for all; if floor: **only sb[0] is used** (`nsb` temporarily 1) and `floor_blk = sb[0].id` (C:507).
5. `want[b] = ceil(need_b*(1+margin))`; `frames = clamp(ceil(Σwant/ppf), 1, n)`.
6. Per frame: (a) in **frame 0 only**, the first block with `resend ≥ 0` gets a segment of `min(2, K-resend)` data pieces starting at `resend`, then `resend = -1` (these do **not** advance `next`, `sent`, `round_sent`) (C:535-547). (b) Fill oldest-block-first: each segment takes pieces `next, next+1, …` (`next = (next+1) mod 256`, `sent = min(sent+1, 256)`, `round_sent++`), up to its want; once every want is met the frame is topped up with further pieces of `sb[0]` (stop if the last segment already was sb[0]) (C:550-574). Max 8 segments, ≤ 63 pieces per segment.
7. Stop when all wants are met or `frames` frames built. Restore `nsb`. Write headers (b0, b1, ctl_deaf bit) (C:579-584).

**P5 `deliver_in_order`** (C:1009-1034): loop at `r = rb[rbase % 8]`: if `!known` stop. `upto = done ? len : min(j*24, len)` with j = length of the contiguous run `got[0..j-1]`. Deliver bytes `[delivered, upto)` from `piece[]` (data pieces are systematic); `delivered = upto`. If `!done` stop; else clear the slot, `rbase++`, continue.

**P6 `take_pieces(m)`** (C:1402-1428): per segment: `off = (blk - rbase) & 15`; `off ≥ 8` → `rx_break = true`, skip ("delivered already"). Slot `r = rb[(rbase+off) % 8]`; if `!known` → `known, K = seg.K, len = K*24 - pad` (**first segment wins**; **[since 2c07bf4]** a later segment that disagrees fails the session). If `done` → `rx_break = true`, skip. Each piece `i = (first+p) mod 256`: if `!got[i] && have < K` → store, `have++`. If `have ≥ K` → `decode_block` (RS decode, then `done = true`, `done_in_round++` **regardless of decode result**, C:986-1000; **[since 2c07bf4]** a decode failure or a failed block check fails the session, and nothing more is delivered), `rx_break = true`. After all segments: `peer_unopened = m.unopened; peer_hi = resolve16(rbase, m.hi); peer_hi_known = true; peer_snr_level = m.snr_level`.

**P7 `resolve16(base, v4)`** = `base + d`, `d = (v4 - base) & 15`, `d ≥ 8 → d -= 16` (C:333-338): nearest id in `[base-8, base+7]`.

**P8 `peer_outstanding`** = `!peer_hi_known ? -1 : max(0, (int8)(peer_hi - rbase) + 1)` (C:940-945).
**`peer_direction_done`** = `status_seen || (!peer_unopened && peer_outstanding == 0)` (C:1037-1040).

**P9 `defer_if_busy(t)`** (LBT, C:372-380): if `tx_busy || peer_keyed()` → `t := now + 250`, true. Else if `last_carrier_ms && now < last_carrier_ms + 700` → `t := last_carrier_ms + 700`, true. Else false.

**P10 `start_driving(pid, lv, n, round_start, now)`** (C:1066-1086): `sending = idle = handover_unconfirmed = false`; `poll_id = pid; poll_level = lv; poll_n = n; drove_peer = true`; Bind(lv); `round_seen = 0; round_frames = n; status_seen = false; round_heard = true; done_in_round = 0; drive_start = now; rx_break = false; floor_patterns = 0; floor_streaming = probe_off_floor = floor_fallback = floor_hold = floor_waiting = pat_waiting = false`; disarm SEND, WAIT, SENSE; `T_POLL := round_start + H + round_air(lv,n) + T + WM + G`.

### 5.2 Session start (connect)

| # | Rule | Cite |
|---|---|---|
| S1 | Callee on CALL (LISTENING): `peer_carousel = CALL marked`; `car_rx_level = car_start_level(CALL SNR)`; `car_ctl_deaf = CALL SNR < -7`; `car_peer_ctl_deaf = false`; ACCEPT after `G`. A repeated CALL in ACCEPTING recomputes these and answers on the CALL's carrier. | F:1600-1631, F:1953-1984 |
| S2 | Callee sending a carousel ACCEPT: ACCEPT level = `car_ctl_deaf ? 7 : car_rx_level`; **seed set now**; payload decoder bound to `LADDER[car_rx_level]`. | F:904-918 |
| S3 | Caller on carousel ACCEPT (CALLING, matching session id): `car_peer_ctl_deaf = (level == 7)`; `car_tx_level = peer_ctl_deaf ? 0 : level`; `car_rx_level = car_start_level(ACCEPT SNR)`; `car_ctl_deaf = ACCEPT SNR < -7`; → CONNECTED; seed set; `car_start(is_caller)`. | F:1745-1760 |
| S4 | `car_start`: `car_init(rx_level, tx_level)` (snr_level = rx_level; peer_snr_level = tx_level ≥ 0 ? tx_level : rx_level; tx_level = -1; loss_est = tx_loss = 0.1); seed ctl_deaf/peer_ctl_deaf; `car_last_rx_ms = now`; caller → S5, callee → S6. | F:1495-1513, C:1553-1561 |
| S5 | `car_start_sender`: `sending = true; send_start_ms = now; tx_level = peer_snr_level; tx_n = 1; tx_poll_id = 1; handover_unconfirmed = true; T_SEND := now`. | C:1568-1575 |
| S6 | Callee: the **first seeded frame of any kind** (ACCEPTING, or LISTENING after the ACCEPT-retry fallback with seed still set) → `car_connect_callee`: `car_tx_level = -1`, → CONNECTED, `car_start(callee)` = `start_driving(1, snr_level, 1, now, now)` (C:1578-1581), then the frame is processed by `car_on_frame`. | F:1869-1893, F:1899-1902, F:1661-1664 |
| S7 | ACCEPT-retry exhaustion: callee → LISTENING with `accept_fallback`, seed kept, decoder kept, for 240 s; then seed cleared. | F:2045-2061, F:1666-1674 |

### 5.3 DATA frame received — `on_data(m, lv)` (C:1430-1462)

| # | Guard | Actions |
|---|---|---|
| D0 | `lv != rx_level` | drop (only `last_carrier_ms`, SNR, ctl_deaf already updated by `car_on_frame`). |
| D1 | `sending` | the peer took the turn with a handover I missed: `start_driving(m.poll_id, lv, m.left+1, now - A(lv) - H, now)`. My open blocks stay suspended. |
| D2 | always (after D1) | `idle = false; round_heard = true; silent_polls = 0`; `take_pieces(m)` (P6). |
| D3 | `lv == 0 && poll_level == 0 && io.pattern` (floor_frame) | `floor_streaming = true`. |
| D4 | `m.poll_id == my_poll_id && polled_since_handover` | `polls_unheard = 0`. |
| D5 | `m.poll_id == poll_id || floor_frame` | `round_seen++; round_frames = round_seen + m.left`. (At the floor frames keep the id of the last poll the sender heard, so any floor frame counts.) |
| D6 | always | `deliver_in_order()`. |
| D7 | `lv > 0` | `peer_round_end = now + left*(A+gap); peer_round_lv = lv; peer_round_n = max(round_frames,1)`. |
| D8 | always | `T_POLL := now + left*(A(lv)+gap(lv)) + T + G`. |

### 5.4 Control frame received — `on_ctl(m)` (C:1464-1522)

Always first: `peer_snr_level = m.snr_level` (C:1466) (and `peer_ctl_deaf = m.ctl_deaf`, C:1617/1621).

**STATUS** (C:1467-1475)

| # | Guard | Actions |
|---|---|---|
| ST1 | `sending` | ignore. |
| ST2 | else | if `m.poll_id == poll_id` → `status_seen = true`; `round_heard = true; silent_polls = 0; peer_hi = resolve16(rbase, m.hi); peer_hi_known = true; peer_unopened = false`; `T_POLL := now + G`. |

**HANDOVER** (C:1476-1491) — accepted in **any** state (idle, driving, sending)

| # | Actions |
|---|---|
| HO1 | `handover_unconfirmed = false`. |
| HO2 | `polls_unheard = (polled_since_handover && (m.poll_id & 15) != (my_poll_id & 15)) ? polls_unheard + 1 : 0`; `polled_since_handover = false`. |
| HO3 | `peer_has_data = m.has_data`; `apply_need(m)` (P3: retires **my** blocks per the peer's receive window). |
| HO4 | `peer_unopened = m.unopened; peer_hi = resolve16(rbase, m.hi); peer_hi_known = true`. |
| HO5 | `start_driving(m.h_id, m.h_level, m.h_n, now + CG - H, now)` (the handover round follows after the 300 ms chain gap). |

**POLL** (C:1492-1522) — accepted in any state

| # | Guard | Actions |
|---|---|---|
| PL1 | always | `handover_unconfirmed = false`; disarm POLL, SENSE, WAIT; `floor_waiting = pat_waiting = false; floor_silent = 0`; `apply_need(m)`; `peer_has_data = m.has_data; floor_pats_heard = 0`; if `!sending` → `send_start_ms = now`; `tx_poll_id = m.poll_id; heard_poll_id = m.poll_id & 15; tx_loss = m.loss16/15`. |
| PL2 | `m.n == 0` ("ack only, the peer is done with us") | `sending = false`; if `!has_data` → `idle = true`, stop. Else `tx_n = 1; T_SEND := now + 900` (tx_level unchanged). |
| PL3 | `m.n > 0` | `sending = true; idle = false; tx_level = m.level; tx_n = m.n`; Bind(`peer_ctl_on_floor ? 0 : drove_peer ? poll_level : m.level`) — listen for the peer's possible handover round on the rung **I** last polled it on; `T_SEND := now + 900`. |

### 5.5 Pattern received — `car_on_pattern(kind)` (C:1681-1728)

| # | Guard | Actions |
|---|---|---|
| PA0 | always | `last_carrier_ms = now; floor_yielded = false; floor_pats_heard++` (F:2316 also sets `car_last_rx_ms`). |
| PA1 | `!sending || tx_busy || !(floor_waiting || pat_waiting)` | ignore. |
| PA2 | `pat_waiting` (above floor; kind ignored) | `pat_waiting = false; handover_unconfirmed = false`; disarm WAIT; for each open block `need = max(0, need - round_sent); round_sent = 0` (**no retirement**); if `!has_data` → `sending = false; idle = true`; else `T_SEND := now + 900` (same `tx_level`, `tx_n`). |
| PA3 | `floor_waiting` | `floor_waiting = false; floor_silent = 0`; if `floor_tx_end && now > floor_tx_end`: `d = now - floor_tx_end; floor_delay_ms = floor_delay_ms ? (3*floor_delay_ms + d)/4 : d`; `handover_unconfirmed = false; tx_n = keydown_cap(0) (= 2)`; disarm WAIT. |
| PA4 | PA3 and `kind == BREAK`, for the open block with `id == floor_blk` and `sent ≥ K` | **[since 2c07bf4]** **stop it**: `stopped = true; need = 0; resend = -1`. Nothing is retired: only a POLL/HANDOVER does (P3), which also clears `stopped` on a block it still needs. A floor round carries the oldest block not stopped, or, every one stopped, repair for the oldest; a new block opens at the floor only when no open block is unstopped. |
| PA5 | PA3, then | if `!has_data` → `sending = false; idle = true`; else `T_SEND := now + 900`. |

### 5.6 CAR_T_POLL fires — `on_poll_timer` (C:1209-1371), evaluated in order

| # | Guard | Actions |
|---|---|---|
| T0 | `sending || idle` | return. |
| T1 | `(floor_fallback || floor_hold) && peer_keyed()` | `round_heard = true` (continue). |
| T2 | `defer_if_busy(T_POLL)` | return (re-armed). |
| T3 | `clear_of_peer_nudge()` (C:1189-1205): `!sending && peer_round_lv > 0 && peer_round_end && !(last_carrier_ms > peer_round_end + G)`; `start = peer_round_end + 90000; end = start + H + round_air(peer_round_lv, peer_round_n) + T; mine = now + G + H + ctl + T`; true unless `now ≥ end + G || mine + G ≤ start` | Bind(`peer_round_lv`); `peer_round_end = end`; `T_POLL := end + G`; return. |
| T4 | **floor fallback**: `probe_off_floor && !floor_fallback && !round_seen && !status_seen && !round_heard` | `floor_fallback = true; poll_level = 0; poll_n = 2`; Bind(0); `round_seen = 0; round_frames = 2; round_heard = false; done_in_round = 0`; disarm SENSE; `T_POLL := max(now + 13700, probe_after_ms + peer_floor_wait_max + H) + 3000`; return. |
| T5 | **floor hold**: `poll_level == 0 && io.pattern && !floor_hold && !floor_fallback && !peer_ctl_deaf && !round_seen && !status_seen && !round_heard` and `until = floor_round_end + peer_floor_wait_max + 3000 > now` | `floor_hold = true; T_POLL := until`; return. |
| T6 | always | `floor_hold = false`. |
| T7 | **unscored re-poll**: `!floor_fallback && poll_level > 0 && !round_seen && !status_seen && !round_heard && round_frames > 0 && silent_polls == 0 && lv_sent[poll_level] ≥ 2 && level_delivery(poll_level) ≥ 0.5` | `silent_polls = 1`; `send_poll_as(poll_level, poll_n, repeat = true)` (same poll id); return. |
| T8 | measure | `probe_failed = floor_fallback && !round_seen && !status_seen && !round_heard`; `probe_off_floor = floor_fallback = false`. If probe_failed: `loss_est = 0.5*loss_est + 0.5`; `measure_level(probe_lv, probe_n, 1.0)`. Else if `!status_seen`: `frames = max(round_frames,1)`; `loss = max(0, 1 - round_seen/frames)`; `loss_est = 0.5*loss_est + 0.5*loss`; `measure_level(poll_level, frames, loss)`. |
| T9 | always | `deliver_in_order()`. |
| T10 | **handover**: `has_data && (done || quantum)` where `done = peer_direction_done`, `held = now - drive_start`, `quantum = (done_in_round > 0 && held ≥ 30000) || held ≥ 45000` | `send_start_ms = now; send_handover()`; return. |
| T11 | **done**: `done` (and no own data) | `idle = true`; `send_poll(0, 0)` (ack-only) -- **[since 2c07bf4]** at the floor too (it was a BREAK, which no longer retires). Return. |
| T12 | choose | `lv = choose_level(now)` (5.10); if `floor && lv > 0 && !(snr_valid && snr_ema ≥ -7)` → `lv = 0`. |
| T13 | | if `round_seen > 0 && poll_level == 0` → `floor_streaming = true`. |
| T14 | **floor pattern**: `floor && lv == 0 && floor_streaming && floor_patterns < floor_poll_every()` (12 if `snr_valid && snr_ema < -10 && !has_data`, else 4; C:1157-1164), **[since 2c07bf4]** and not `window_full` (`peer_hi + 1 - polled_base ≥ 6`, polled_base = the base of my last poll/handover) and not `stuck` (two rounds in a row with no piece of a block I lack: the sender holds only stopped blocks) | `floor_patterns++`; `send_pattern(rx_break ? BREAK : ACK, expect_round = true)`; return. |
| T15 | | `n = poll_size(lv, now)` (5.10). |
| T16 | **above-floor pattern**: `io.pattern && lv > 0 && lv == poll_level && (n == poll_n || polls_unheard > 0) && !status_seen && round_seen > 0 && round_seen ≥ round_frames && floor_patterns < 4` | `floor_patterns++; pattern_for_lost_polls = (polls_unheard > 0)`; `send_pattern(ACK, expect_round = true)`; return. |
| T17 | otherwise | `send_poll(lv, n)`. |

**send_poll_as(lv, n, repeat)** (C:1121-1151): `fill_poll`; type POLL; if `!repeat` → `poll_id = (poll_id+1) & 15`; `m.poll_id = poll_id; m.level = lv; m.n = n`. If `!repeat`: `probe_off_floor = (n && lv > 0 && poll_level == 0 && floor_streaming && io.pattern)`; `floor_fallback = false`; if probe_off_floor → `probe_lv = lv; probe_n = n; probe_after_ms = last_carrier_ms`. Then `rx_break = false; floor_patterns = 0; floor_streaming = false`. If `n > 0`: `poll_level = lv; poll_n = n; drove_peer = true`; Bind(lv); `polled_since_handover = true; my_poll_id = poll_id`. `round_seen = 0; round_frames = n; status_seen = false; done_in_round = 0; round_heard = false; floor_hold = false`; disarm POLL, SENSE; keydown(1 control frame, after = n ? AFTER_POLL : AFTER_NONE).

**send_pattern(kind, expect_round)** (C:1166-1180): `rx_break = false`; if expect_round: (if `poll_level == 0` → `poll_n = 2`); `round_seen = 0; round_frames = poll_n; status_seen = false; done_in_round = 0; round_heard = false`. `floor_hold = false`; disarm POLL, SENSE; `tx_busy = true; after_tx = expect_round ? AFTER_PATTERN : AFTER_NONE`; `io.pattern(kind)`.

**send_handover()** (C:1089-1119): `fill_poll`, type HANDOVER; `sending = true; idle = false; tx_poll_id = (tx_poll_id+1) & 15`; `lv = tx_level ≥ 0 ? tx_level : peer_snr_level; n = tx_n > 0 ? tx_n : 1; tx_level = lv; tx_n = n`; `nd = build_round(lv, n, tx_poll_id)` into frames 1..; header `h_level = lv, h_n = nd, h_id = tx_poll_id, hi, unopened, poll_id = heard_poll_id`; frame 0 = control (put_ctl); if `nd` → frame 1 gap = 300; `handover_unconfirmed = true`; disarm POLL, SENSE; keydown(1+nd, AFTER_ROUND).

### 5.7 CAR_T_SENSE fires — `on_sense_timer` (C:1387-1400)

| # | Guard | Actions |
|---|---|---|
| SE1 | `sending || idle || round_seen > 0` | return. |
| SE2 | `poll_level == 0 && io.pattern` | return (never at the floor). |
| SE3 | `peer_keyed()` | `round_heard = true; silent_polls = 0; T_SENSE := now + 250`. |
| SE4 | else if `round_heard` (carrier was seen, now dropped) | `T_POLL := now + G`. |
| SE5 | else | nothing (the window T_POLL decides). |

### 5.8 CAR_T_SEND / CAR_T_WAIT fire (C:1756-1792)

**SEND:** if `defer_if_busy` → return; `sending = true`; `send_round()` (C:672-687): `floor_yielded = false`; `nf = build_round(tx_level, tx_n, tx_poll_id)`; if `nf == 0` → one STATUS control frame (`poll_id = tx_poll_id, hi, snr_level, ctl_deaf`); keydown(AFTER_ROUND).

**WAIT** (rule W):

| # | Guard | Actions |
|---|---|---|
| W0 | `!sending` | nothing. |
| W1 | `defer_if_busy(T_WAIT)` | return. |
| W2 | `handover_unconfirmed` | `send_handover()` (repeat; fresh pieces). |
| W3 | `floor_waiting && peer_has_data && !floor_yielded && now + 5000 ≥ send_start_ms + 30000` | `floor_yielded = true; T_WAIT := now + 90000` (hold for the peer's handover). |
| W4 | `floor_waiting && ++floor_silent > 6` | `floor_waiting = false; T_WAIT := now + 90000` (stop streaming). Note `floor_silent` is reset only by a poll or pattern (C:1495, C:1707), so subsequent silent floor rounds are 90 s apart. |
| W5 | otherwise | `send_round()` — floor: continue on silence; above floor: the 90 s silence nudge. |

### 5.9 Keydown end — `car_on_tx_done` (C:1642-1679)

| # | Guard | Actions |
|---|---|---|
| X0 | always | `tx_busy = false; after = after_tx; after_tx = NONE`; `take_turn_if_idle(now)` (5.11). |
| X1 | `after == AFTER_ROUND` | `arm_sender_wait(now)` (4.3). |
| X2 | `after == AFTER_PATTERN` | `start = now - 200 + 900`; `floor = (poll_level == 0)`; if `!floor && !pattern_for_lost_polls` → `T_SENSE := start + 3400`; `floor_round_end = start + H + round_air(poll_level, poll_n) + T`; `T_POLL := floor_round_end + 1000 + (floor ? 9000 : 0) + G`. |
| X3 | `after == AFTER_POLL` | `start = now + 700`; `floor = poll_level == 0 && io.pattern`; `T_SENSE := start + 1400 + (floor ? 3000 : 0)`; `floor_round_end` as X2; `T_POLL` as X2. |
| X4 | `AFTER_NONE` (ack-only poll, final BREAK) | nothing. |

### 5.10 Link adaptation (receiver side, summarized precisely)

`measure_level(lv, frames, loss, now)` (C:751-768): `lv_sent = 0.8*lv_sent + frames`;
`lv_lost = 0.8*lv_lost + frames*loss`; `lv_dead_run = (loss ≥ 1) ? lv_dead_run + frames : 0`;
`lv_probe_fails = (loss ≥ 0.5) ? +1 : 0`; `lv_rounds++`; `lv_round_at = ++polls`;
`lv_probe_at = now`; `lv_backoff_ms = loss ≥ 0.9 ? (0 → 60000, else ×2 capped 480000) : 0`.

Predicates (`marginal = snr_valid && snr_ema < min_db + 3`, C:797-800):
- `delivery(lv)` = marginal ? `(sent - lost)/(sent + 1)` : `1 - (lost + 0.5)/(sent + 2)` (C:737-742).
- `dead(lv)` = `dead_run ≥ (marginal ? 2 : 4)` (C:746-749).
- `gated(lv)` = `snr_valid && snr_ema < min_db - 3 && !(sent ≥ 3 && delivery ≥ 0.7)` (C:802-806).
- `allowed(lv)` = `!dead && !gated` and, if any lower rung is dead, `sent - lost ≥ 0.5` (C:808-815).
- `reprobe_due(lv)` = `!(rounds == 0 && gated)` and `now - probe_at ≥ (backoff ? backoff : 60000)` (C:821-829).
- `goodput(lv)` = `rate(lv) * delivery(lv) * air/(air + overhead)` with `rate = ppf*24/A`, frames = `min(1 + (int)(sent - lost), keydown_cap)`, `overhead = (lv == 0 && io.pattern) ? 2550 : 6310` (C:707-710, 848-870). `potential(lv)` uses full keydown_cap and delivery 1 (C:872).
- `probe_every(lv)` = 8 unless `marginal && delivery < 0.5`, then `8 << min(probe_fails, 3)` (C:883-892).

`choose_level(now)` (C:894-936):
1. `best = argmax goodput` over rungs with `rounds > 0 && allowed` (ties → lower rung).
2. None: `snr_level` if never tried; else the highest untried rung in `(0, snr_level)`; else 0.
3. If `best_gp ≤ 0`: highest untried rung below best (≥ 0), if any.
4. If `delivery(best) < 0.5` → best.
5. For `up = best+1 .. 6`: skip if `potential(up) ≤ best_gp`; if `!allowed(up)`: stop (→ best) unless `reprobe_due(up)` → return up; else if `rounds[up] > 0 && polls - round_at[up] < probe_every(up)` → stop (→ best); else return up (a probe).
6. → best. Then T12's floor-exit guard.

`poll_size(lv, now)` (C:952-983): `cap = min(keydown_cap(lv), 1 + (int)(sent - lost))`; if `has_data` (I want the turn): `left = 45000 - (now - drive_start)`, `fit = left > 0 ? left/(A+gap) : 1`, `cap = min(cap, max(fit,1))`; if `peer_outstanding ≥ 0 && !peer_unopened` and every block from rbase up to it is known: `cap = min(cap, ceil(Σ_{not done} ceil((K-have)*(1+loss_est*1.3+0.05)) / ppf))`. Result ≥ 1.

### 5.11 Application data and idle

- `car_on_app_data` → `take_turn_if_idle` (C:1735-1738).
- `take_turn_if_idle(now)` (C:1630-1640): guard `idle && !tx_busy && has_data` → `idle = false; sending = true; send_start_ms = now; handover_unconfirmed = true`; disarm POLL, SENSE, SEND; `T_WAIT := now` → W2 sends a HANDOVER after LBT.
- Also called at every `tx_done` (X0), so data queued during my final ack-only poll is not stranded.
- Otherwise new app data is only noticed (a) by `build_round` (sender opens blocks), (b) by `has_data` in `fill_poll` and T10 (driver hands over).

### 5.12 Session end, watchdogs, FSM glue

| # | Rule | Cite |
|---|---|---|
| E1 | `idle` is set only by: T11 (receiver: peer direction done and no own data), PL2 (sender: ack-only poll and no own data), PA2/PA5 (sender: pattern and no own data). Cleared by on_data, take_turn, start_driving, send_handover, PL3. | C:1320, 1505, 1702, 1726 |
| E2 | Lost peer: on TIMER_CAROUSEL, if `!idle && now - car_last_rx_ms ≥ 240000` → session ends locally (no DISCONNECT on air). `car_last_rx_ms` is refreshed only by seeded frames and patterns, not carrier. | F:2323-2332, F:2308, F:2316 |
| E3 | APP_DISCONNECT (not abort): if `!car_drained || tx_active` → `pending_disconnect`, deadline `now + max(45 s, car_drain_budget_ms)`; `car_drained = idle && tx backlog == 0`. After every carousel event, if pending && drained && !tx_active → DISCONNECTING. Abort → DISCONNECTING immediately. | F:2335-2365, F:2373-2379, F:1524-1527 |
| E4 | `car_drain_budget_ms` = `3 × (G + H + ctl + CG + round_air(lv, keydown_cap(lv)) + T + 4000)`, `lv = min(tx_level (or 0), poll_level if drove_peer)`, `ctl = 13500` if either end is control-deaf (and MFSK longer) else 3740. | C:1801-1810 |
| E5 | Drain deadline elapsed → forced DISCONNECTING (data may be undelivered). | F:2391-2405 |
| E6 | Leaving CONNECTED: `car_active = false`, `ctl_floor_at_stop` remembered for the DISCONNECT carrier; seed cleared only on DISCONNECTED/LISTENING. Carousel frames in DISCONNECTING are ignored. | F:191-203, F:3953 |
| E7 | RX DISCONNECT (session id matches) in CONNECTED → DISCONNECTED immediately (reply after G); whatever the carousel held is dropped. | F:2508-2522 |
| E8 | LISTEN OFF in CONNECTED → idle immediately, no DISCONNECT on air. | F:2413-2428 |
| E9 | KEEPALIVE and late ACCEPT are swallowed while the carousel runs. | F:2366-2368 |

---

## 6. Keydown origins: response vs blind

"Response" = keyed because of a frame/pattern just decoded from the peer (the
peer is known to be listening now). "Blind" = keyed on a timer whose last arm did
not come from hearing the peer's current transmission end. Every keydown is
timer-initiated and passes `defer_if_busy` (LBT) first — T2, SEND, W1 — **except none**:
there is no direct keying from an RX handler (C:1213, 1757, 1766; send_* only called from those paths).

| # | Keydown | Origin | Class |
|---|---|---|---|
| K1 | caller's first round | S5: `T_SEND := now` at carousel ACCEPT (C:1574) | response to ACCEPT, **but no G guard**: `last_carrier_ms = 0` after `car_init`, so LBT is only the 250 ms sync hold / preamble hold (see R9) |
| K2 | round after POLL | PL3 → SEND +900 | response |
| K3 | STATUS | SEND after POLL with nothing to send (C:677-685) | response |
| K4 | round after floor ACK/BREAK | PA5 → SEND +900 | response |
| K5 | round after above-floor ACK | PA2 → SEND +900 | response |
| K6 | 1-frame round after ack-only POLL while I have new data | PL2 → SEND +900 | response, but unsolicited: the receiver is idle; heard only because its decoder is still bound to that rung |
| K7 | floor round on silence | W5 with `floor_waiting` | **blind** (silence = "keep going"; up to 6 in a row) |
| K8 | above-floor silence nudge | W5, `T_WAIT = tx_end + 90000` | **blind** |
| K9 | HANDOVER repeat | W2, `handover_unconfirmed` (includes the caller's unconfirmed first round) | **blind** |
| K10 | HANDOVER to take the turn from idle | take_turn_if_idle → `T_WAIT := now` → W2 | **blind / unsolicited** (both idle ends can do this simultaneously, R10) |
| K11 | HANDOVER at round end | T10 | response if `T_POLL` was last armed by D8/ST2/SE4; **blind** if it fired from a window (X2/X3/P10/T4/T5) with nothing heard |
| K12 | POLL | T17 | same split as K11 |
| K13 | repeat POLL | T7 (nothing heard in the window) | **blind** |
| K14 | floor ACK/BREAK pattern | T14 | response, or **blind** when the round was lost (a stream is answered regardless, C:1332-1338) |
| K15 | above-floor ACK pattern | T16 (requires `round_seen ≥ round_frames > 0`) | response |
| K16 | final ack-only POLL / final BREAK | T11 | response to the round/STATUS that completed the direction (window-timed) |
| K17 | DISCONNECT (FSM) | DISCONNECTING after G and `disconnect_must_wait` | blind w.r.t. the carousel (F:2132-2167) |

Precision note for modelling: whether K11/K12/K14 are responses depends on
which rule last set `T_POLL`. A model should carry a ghost variable
`poll_armed_by ∈ {FRAME, STATUS, CARRIER_DROP, WINDOW, NUDGE_AVOID, FALLBACK, HOLD, DEFER}`.

---

## 7. Invariants and timing assumptions

### 7.1 Invariants the code maintains or relies on

| # | Invariant | Maintained / relied on at |
|---|---|---|
| I1 | **Sender id span**: `(uint8)(next_blk_id - sb[0].id) ≤ 8` (a block is opened only while the span is < 8). | C:498-499, comment C:481-486 |
| I2 | **Receiver/sender window alignment**: absent a wrong BREAK, `sb[0].id ≤ rbase ≤ sb[0].id + 8` (sender retires only what the receiver reports need 0 or behind base; rbase only slides over done blocks). With I1 every in-flight id lies in `[rbase-8, rbase+7]`, so mod-16 resolution (P7, apply_need, take_pieces) is exact. | C:457-475, 1009-1034, 1406-1407 |
| I3 | Ids and `rbase` are uint8 and wrap at 256; 256 is a multiple of 16 and of 8, so `id mod 16` and slot `id % 8` are consistent across the wrap. | H:124, H:157, C:1013 |
| I4 | Piece indices are mod 256; K ≤ 96 ⇒ ≥ 160 distinct repair indices before `next` wraps to already-sent indices. `sent` counts distinct pieces up to 256; resend-gap pieces are not counted (repeats). | C:563-565, C:46-50 |
| I5 | **Floor rounds carry only `sb[0]`**, so a BREAK can only name it; the sender records `floor_blk`. | C:494-496, 507 |
| I6 | **[since 2c07bf4]** **BREAK never retires**: it stops the block `floor_blk` if `sent ≥ K`. Retirement is by POLL/HANDOVER only, so I2 holds whatever patterns arrive. | PA4 |
| I7 | Above-floor ACK pattern never retires; it only lowers `need` (a wrong ACK costs pieces, not data). Retirement otherwise only via a POLL/HANDOVER `need`/`base` (P3). | C:1689-1701 |
| I8 | Data is delivered to the app strictly in order and exactly once: `delivered` is monotone per block; slot cleared only when `done` and fully delivered; `rbase` advances by one. | C:1009-1034 |
| I9 | A receiver accepts DATA only on its bound rung (`lv == rx_level`); counts a frame toward the round only if its poll id matches (or at the floor). | C:1432, 1451 |
| I10 | At most one own transmission at a time (`tx_busy`), and every keydown passes LBT (P9) — guard 700 ms after the last decoded frame/pattern/carrier. | C:372-387 |
| I11 | Exactly one of: `sending` (sender), driving (`!sending && !idle`), `idle`. Handover makes the receiver the sender **before** the peer knows (`sending = true` at C:1095); the peer becomes driver on HO5 or D1. | — |
| I12 | `handover_unconfirmed` ⇒ the WAIT timer is the short answer window and its firing repeats the HANDOVER; cleared by any POLL, HANDOVER, or accepted pattern. | C:657, 1767, 1476, 1695, 1712 |
| I13 | A poll repeat keeps its id, so frames of a round answering the first ask still count. | C:1127-1131 |
| I14 | At the floor a sender continues on silence at most `FLOOR_SILENT_MAX` rounds, then only every 90 s; a receiver polls at least every 4 (12 deep) floor rounds. | C:1785-1789, C:1157-1164 |
| I15 | Control frames go on MFSK iff the peer reports `ctl_deaf` (both ends act on the same reported bit). | C:169-192 |

### 7.2 Where correctness/liveness depends on timing

| # | Dependency | Cite |
|---|---|---|
| TA1 | `HEAD_MS = 110` models TX delay + head silence; `T = 200` the tail; all windows assume the peer keys `IG = 900` (or `CG = 300` chained) after the event. | C:67-71 |
| TA2 | Frame airtimes from `arq_mode_table` must match the modem (rounds are predicted, not observed). | P:91-105 |
| TA3 | Carrier sense latency: OFDM rounds must be sensed by `S = 1400 ms` (+2000 after a pattern; +3000 MFSK), else the receiver relies on the window; MFSK decode ends ~3.7 s after unkey (FLOOR_ANSWER 4000). | C:98-121 |
| TA4 | Rebinding the payload decoder (`bind_rx`) completes within the 300 ms chain gap after a HANDOVER decodes. | C:1514-1519 |
| TA5 | The floor windows (`FLOOR_LATE_MS 9000`, `peer_floor_wait_max`) must exceed the peer's `arm_sender_wait` answer, i.e. the two ends' formulas are designed as a pair; the receiver uses **its own** view of `ctl_on_floor` to compute the sender's wait (C:665-670 vs C:594). | C:661-670 |
| TA6 | Clear-of-nudge: the receiver predicts the sender's 90 s repeat from the last round heard (assumes identical constants on both ends). | C:1182-1205 |
| TA7 | Turn quantum (30/45 s) must stay well under application silence timeouts (NNCP 120 s). | C:55-65 |
| TA8 | Lost-peer 240 s ≫ longest exchange (~40-50 s at the floor). | F:1391 |
| TA9 | The caller's first round assumes the callee has unkeyed within the 250 ms sync hold after the ACCEPT decodes (R9). | C:1574, F:1245-1258 |

---

## 8. Known gaps and risks

| # | Risk | Detail | Cite |
|---|---|---|---|
| R1 | **Integrity = one CRC16 per frame** — **fixed [since 2c07bf4]**: a CRC-32 per block, checked after decode; a failure ends the session (fail closed) | Seeded CRC16 is the only check; an undetected corrupted frame (~2^-16 per synced bad frame) puts a wrong piece into a block. No per-block or end-to-end checksum; RS decode does not verify. | M:1192, C:986-1000 |
| R2 | **Streaming delivery before block decode** | Data pieces are handed to the app as soon as the contiguous prefix arrives (P5); a bad piece is delivered irrevocably, and a later RS decode (which overwrites `piece[0..K-1]`) cannot correct what was already delivered — the delivered prefix and the decoded block may silently disagree. | C:1015-1029 |
| R3 | `decode_block` ignores `rs_decode` failure — **fixed**: it fails the session | `done = true` and the block is delivered even if `rs_decode` returned -1 (slots never filled stay zero). Unreachable for distinct indices in theory, but reachable with an inconsistent K (R4). | C:996-998 |
| R4 | Block metadata from the first segment — **fixed**: two segments that disagree on K or length fail the session (the first may be the bad one) | `K` and `len` (pad) are taken from the first segment seen for a slot and never cross-checked against later segments; a mis-resolved id (R6/R7) mixes two blocks' pieces with the wrong K. | C:1409-1412 |
| R5 | **Patterns are unauthenticated** — **fixed for data [since 2c07bf4]**: a BREAK no longer retires (PA4); see R23 for what a spurious pattern can still do | A pattern carries no seed/session; any station's pattern or a detector false alarm while `floor_waiting` with `sb[0].sent ≥ K` retires `sb[0]` (PA4). That block is then never resent: the receiver's base stalls forever; worse, once the sender retires the rest it answers a poll with STATUS and the receiver declares the direction done (`status_seen`) with the block missing → **silent truncation reported as success**. | C:1719-1725, C:1037-1040, C:1469 |
| R6 | Wrong BREAK breaks I2 — **fixed** with R5 | After a premature retirement `sb[0].id > rbase`, the sender may open `id = rbase + 8`, which the receiver resolves as "behind base, delivered" (off ≥ 8) → `rx_break = true` → more BREAKs → cascading retirement of undelivered blocks. | C:498-499, C:1406-1407 |
| R7 | mod-16 ids everywhere | Block ids (4 bits) in DATA/POLL/HANDOVER/STATUS rely on I1/I2; poll ids (4 bits) rely on rounds not being heard 16 polls late. `hi` resolution (`resolve16`) assumes `hi ∈ [rbase-8, rbase+7]`. | C:333-338, 1425, 1449-1451 |
| R8 | `rx_break` is round-scoped, not block-scoped | Set by any segment of a delivered/done block or any completion since the last poll/pattern; correct only because floor rounds carry one block (I5) and `send_poll`/`send_pattern`/`start_driving` reset it. A floor ACK/BREAK sent for a **lost** round (K14) reuses whatever `rx_break` remains. | C:1080, 1141, 1168, 1343, 1407, 1413, 1422 |
| R9 | Caller's first round without channel guard | `car_init` zeroes `last_carrier_ms`, and S5 arms SEND at `now`; LBT then only sees the 250 ms sync hold / preamble hold after the ACCEPT decodes (decode fires ~200 ms before the callee's PTT-off). | C:1555, 1574, F:1245-1258, P:211-218 |
| R10 | Symmetric blind handovers — **reduced [step 3]**: below 0 dB a blind keydown is heard by its NAV header ~1 s after it starts; two inside that second still meet (R24) | Two idle ends that both get app data near-simultaneously both run K10; if keyed within carrier-sense latency they collide, and their WAIT repeats are computed by the same formula from similar tx_end → can repeat in lockstep. Escape: hearing each other (HANDOVER is accepted in any state) or E2's 240 s watchdog. The same holds between a sender's 90 s silence repeat (K8) and the receiver's handover repeat: `tests/sim/car_explore.c` finds them 0.3 s apart with four frames lost (specs/carousel/README.md). | C:1630-1640, 592-658, 1767 |
| R11 | Hidden-terminal collisions remain possible — **mostly fixed [step 3]**: the NAV header is heard ~10 dB below DATAC16 and needs no decoder bound to a mode; sim overlaps 57 → 2 | LBT is decoder sync; a peer below sync threshold, or in a mode not bound, is invisible (A6). Many timing heuristics (T3-T5, W3, arm_sender_wait floor branch) are collision patches tuned in sim/on-air. | F:1220-1244 |
| R12 | Data on an unbound rung is lost and scored | D0 drops frames on another rung; a round sent on a rung the receiver is no longer bound to (lost poll, fallback) costs a full round and is measured as loss. | C:1432, 1282-1293 |
| R13 | SNR estimate mixes carriers | `snr_ema` averages control (DATAC16), MFSK and payload-mode estimates; per-mode estimator bias feeds `ctl_deaf`, `marginal`, `gated`, the floor-exit guard. | C:1607-1615 |
| R14 | STATUS trust | `status_seen` alone ends a direction; it requires only a matching 4-bit poll id. | C:1469, 1039 |
| R15 | Loss under-estimate | `round_frames = round_seen + left` comes from the last frame decoded; losing the tail of a round makes it look shorter (loss under-counted). | C:1453, 1289 |
| R16 | Idle with peer still holding blocks | The final ack-only POLL / BREAK (K16) is unacknowledged; if lost, the sender keeps its blocks unconfirmed and nudges every 90 s (K8) — harmless for data but delays `car_drained` on the sender and its BUFFER never reaches 0. | C:1318-1323, 657 |
| R17 | Seed collisions — **fixed [since 2c07bf4]**: the callee's 16-bit nonce goes into the seed (p ≈ 2^-16, and a stale frame that passes still fails its block's check) | `session_id = (ms & 0x7F) | 1` (64 values); a redial between the same two callsigns reuses the same seed with p = 1/64, so late frames of the old session are accepted as the new one's (block ids restart at 0 → aliasing into the new window). Seeds of unrelated sessions collide with p ≈ 2^-16. | F:1561, F:1461-1480 |
| R18 | Callee connects on any seeded frame | Including a stale frame of a previous same-seed session (R17); and in LISTENING fallback for 240 s. | F:1661-1664, 2053-2061 |
| R19 | `decode_ctl` field validation — **partly fixed**: `need[o] > 96` and `gap ≥ 96` drop the frame | Only type, level, h_level, snr_level ranges are checked; `need[o] > K`, `n > keydown_cap`, reserved bits are accepted. | C:262-291 |
| R20 | After FLOOR_SILENT_MAX, floor sender sends one round per 90 s until a poll/pattern; `floor_silent` never decays. | C:1785-1789 |
| R21 | `drove_peer` never resets | After the first poll it permanently selects `poll_level` (possibly stale) for binding while sending (PL3) and the extra floor wait in `arm_sender_wait`. | C:1072, 1143, 605, 1520 |
| R22 | Runtime clamps silently | `car_io_keydown` clamps frame count to 17 and frame length to 1280 without telling the carousel (fits today's constants). | F:1393-1409 |
| R23 | **[found since]** Two senders after a spurious pattern — **reduced [step 3]**: another station's patterns no longer read as the peer's; a detector false alarm (session ACK/BREAK) still could | A handover keydown lost whole, and a pattern nobody sent heard just after it: the station takes it as the peer's answer (`handover_unconfirmed = false`) and streams, while the peer, which never heard the handover, keeps sending too. Each sends on a rung the other's payload decoder is not bound to, nobody polls, and the session never completes; the 240 s lost-peer watchdog (E2) ends it. Data stays intact. `test_car_explore brkreplay 1 cliff:-5 3 4`. | PA3 |
| R24 | **[found since]** Blind timers coincide — **open**: the 2 sim overlaps left after NAV are this (0.9 s apart, inside a header's detection time) | A sender's 90 s silence repeat and the receiver's handover repeat keyed 0.3 s apart, under carrier-sense latency (four frames lost). Data intact. `test_car_explore replay 1 5 9 10 11` on cliff:-5. Generalises R10. | K8, W-repeat |

---

## 9. Code vs `docs/CAROUSEL-ARQ.md` (code is normative)

Corrected in CAROUSEL-ARQ.md together with this spec.

| Topic | Doc says | Code does |
|---|---|---|
| Re-poll on silence | "poll again at once, and do not score the mode" | No immediate re-poll; T_SENSE only follows carrier (C:1373-1386). One unscored repeat after the window, only on a rung that has delivered (T7); otherwise silence is scored (T8). |
| Turn length | "at most 120 s" | 30 s quantum after a completed block, 45 s cap (C:60-65); poll sizes are also trimmed to fit the cap (C:957-966). |
| POLL need field | bytes 3-10, 255 = none seen | 8 × 7 bits in b3..b9, 127 = none; b10 = in-order gap; b0 bit 0 = ctl_deaf (C:19-29, 233-237). |
| Floor | not described | MFSK rung 0, patterns, MFSK control behind 0x07, ctl_deaf reporting (C:169-192, sections 5.5-5.8). |

---

## 10. Modelling notes (for a TLA+ abstraction)

- State per station = section 2 fields; channel = at most one transmission per station at a time, frames each independently {delivered, erased}; a frame is only decodable by the peer if `frame.mode ∈ {DATAC16, LADDER[peer.rx_level]}` at its start (A2) and the peer is not itself transmitting (A1).
- Patterns: separate channel symbol, observable only while `car_expect_pattern(peer)`; to check R5, allow a nondeterministic spurious pattern action.
- Timers: model deadlines symbolically (ordering only) or with a discrete clock using the constants of 1.3/1.4; the timing-dependent rules (T3-T5, W3, arm_sender_wait floor branch, X2/X3) can be first abstracted as nondeterministic "window expired" actions to check safety (I2, I6, I8, no wrong retirement), then refined to check liveness/collision freedom.
- Safety properties worth stating: (a) delivered stream is a prefix of the sent stream (falsifiable via R2/R5/R6/R17); (b) a block is retired at the sender only if `rb.done` at the receiver; (c) at most one end `sending` after quiescence; (d) `idle` on both ends ⇒ both app queues empty and all retired.
- Liveness: under fair, eventually-reliable channels both directions complete; the 240 s watchdog and drain deadline are the only progress guarantees independent of the channel.

---

## 11. Changes since 2c07bf4 (integrity, branch carousel-integrity)

| Change | Rules | Risks |
|---|---|---|
| A floor BREAK stops a block instead of retiring it; a floor round carries the oldest unstopped block; the receiver acknowledges a finished direction with an ack-only POLL at the floor, polls before the sender's held blocks span the window, and polls when two rounds in a row brought nothing new. | PA4, T11, T14, I6, 3.3 | R5, R6 fixed |
| Every block ends in a 4-byte CRC-32 keyed by the session seed; a block that fails it, or fails to decode, or two segments that disagree on its K/length, fail the session (`car_failed`); the FSM then ends it as ABORT does. The check bytes are never delivered. | 3.1, P6, A3 | R1, R3, R4 fixed |
| The callee's 16-bit nonce in the carousel ACCEPT, hashed into the seed. | 3.4, A4 | R17, R18 fixed (to ≈2^-16) |
| POLL `need`/`gap` range checks. | 3.2 | R19 partly |
| CALL mark `0xA7` → `0xA8` (the block format changed); `0xA9` in step 3 (section 12). | 3.4 | — |

What the stream delivers before a block's check is in can already be wrong when
the check fails: the session ends, so the error is never silent, but those
bytes are with the application.  An application that cannot tolerate that needs
its own check -- or a keyed session (#306), whose AEAD records fail closed the
same way.

---

## 12. Step 3: control signals (branch carousel-signals)

| Change | Where | Risks |
|---|---|---|
| A streaming pattern detector (`mfsk_stream_det`): one FFT per 5 ms step, per-tone energies and peak in a ring one pattern long, every list scored by lookup; bit-identical to the batch detector on the same grid. Cost 8.3 % → about 1 % of an x86 core with 14 lists; on the gateway's Pi 4 the modem used 9-13 % of a core during a 3 % session, as trunk did. | `modem/mfsk/mfsk_sync.c` | makes the rest affordable |
| Session-bound ACK and BREAK: tone lists from a deterministic search keyed by the CRC seed, set by `modem_apply_crc_seed`, so only the session's two stations read each other's patterns. Global lists with no session. | `mfsk_session_patterns`, `mfsk_pattern_set_session` | R5's source (another station's BREAK), R23 |
| NAV header: below 0 dB (`car_wants_nav`) a keydown opens with a 0.64 s pattern of class k = the smallest whose `mfsk_nav_class_ms(k)` (2-32 s, ×1.287) covers the keydown from the header's start, then 60 ms. A station that hears it sets `nav_busy_until_ms` = start + class, OR'd into `peer_is_transmitting`. The carousel's `HEAD_MS` carries the 700 ms lead when it heads its keydowns. `MERCURY_NAV=0` turns it off. | `modem.c` (`send_modulated_keydown`, rx loop), `arq_note_rx_nav`, `carousel.c` | R10, R11 |
| The pattern window runs throughout a carousel session; ACK/BREAK count only while due and from bursts that began after; it is reset after the station's own keydown. Of overlapping events only the strongest is reported, after a two-symbol hold. NAV needs 9 of 16 symbols (ACK/BREAK 8). | `modem_mfsk.c`, `modem.c` | false holds |
| CALL mark `0xA9`: `0xA8` carousels send the global ACK/BREAK. | `arq_protocol.h` | — |

Measured (details in the commit messages):
- carousel_bench, 15 channels × both ways × 20 seeds, pattern cliff at the
  real detector's −14 dB: overlapping keydowns 57 → 2; up to 4 % slower at
  the fringe, nothing above 0 dB.
- pattern_probe: NAV classes and session lists detect like the global ACK
  (NAV at 9 of 16: about 0.7 dB later at −14 dB); 0 false headers in 10 h of
  noise and on every data mode's bursts; 0 cross-reads in 2048 clean session
  ACK/BREAK bursts.
- harness, two-way at −4/−7 dB `mpp`, NAV vs `MERCURY_NAV=0`: 6/6 runs
  passed with no overlaps, against 5/6 (the failure had 2 overlaps).
- on air, gateway ⇄ estacao2 at 3 %: the gateway heard estacao2's headers,
  each as the class sent, about 675 ms after they began.  Threshold 8 let one
  session ACK cross-read as a 32 s NAV (the run lost 150 s), which led to 9.
  At 9, against trunk deab316, interleaved: 2 KB at 3 % in 693/703/740 s vs
  689/653/718 s; 8 KB both ways at 20 % in 362/351 s vs 418/370 s; every
  header heard, none false, no overlaps; the gateway's modem at 9 % of a
  Pi 4 core either way.

Open: R24 (blind timers inside a header's detection time); NAV headers are
not yet sent or honoured outside a session (third-party listen-before-talk).

## 13. Step 4: optimisation against a bound (branch carousel-optimise)

`tests/carousel_bound <channel> [bidir]` gives what the carousel could do
on a sim channel at best, using the carousel's own round geometry
(`car_rung_geometry`: bytes per frame, frames per full keydown, its
airtime, a round's fixed overhead).  It reports two numbers:

- `bound_s`: an oracle that knows each second's frame loss on every rung
  (`sim_channel_frame_per`, no random draw) and is always on the best rung.
  It never probes, never loses a poll and never waits on a timer.  Below a
  mode's cliff it counts frames as lost.  The sim's 10 % survival there is
  an artefact. NVIS is the exception, because its loss table is measured.
- `fixed_s`: the best single rung held throughout.  The oracle follows
  0.5 Hz fades that no round can, so on fades the target lies between the
  two numbers.

Efficiency before this step (8 KB one way, bound / bench):
- cliff channels: 62–93 %;
- fades: 72–95 % of `fixed_s`;
- flat loss (`awgn:`): 32–48 %. Its polls are lost too, which the bound
  ignores;
- NVIS: 29 % of an oracle that picks QAM16C2 at 5 % success. The table is
  synthetic and was not chased.

| Change | Where | Effect (sim, 20 seeds) |
|---|---|---|
| No pattern ACK above the floor to a sender whose window is full. Only a poll retires blocks, so until one came the sender had nothing new to open and sent one-frame repair rounds. | end of round | 64 KB cliff:20 357 → 295 s; cliff:10 674 → 612 s |
| A probe that came whole earns a second (two-frame) round before the argmax judges it. Near its threshold, one good frame scores 1/2, below the rung beneath it. Not used below threshold − 1.5 dB. | `choose_level` | awgn:0.1 90 → 79 s one way, 179 → 154 s both ways; 64 KB 563 → 447 s |
| The receiver keeps its blind polls off the sender's handover repeats: they are predicted from the heard handover with the sender's NAV lead. A ±`SENSE_MS` window applies around the first repeat. Every repeat is waited out only before a turn-cap handover in a turn where nothing of the peer was heard. | `clear_of_peer_handover_repeat` | car_explore cliff:−5 bidir {11,47,48} fixed; collisions over 120 seeds both ways: fade:−3 6 → 1; asym:−9:3 without NAV 28 → 0 |
| Rounds double from a probe (earned = 1 + 2 × delivered) where the SNR has room; near the threshold they still grow by one frame. | `level_earned` | nvis 2217 → 2030 s one way, 4259 → 3506 s both ways; step both ways 579 → 484 s; cliff:20 −10 % |

Rejected after measuring:
- Waiting out every predicted repeat before any poll cost 10–20 % both ways
  at 0..10 dB. Silence after a poll means a lost round as often as a lost
  poll.
- Gating the turn-cap handover on having heard the peer gained little and
  cost 1–2 %.
- Requiring a delivery before probing up: fades −2..−6 %, but flat loss
  +6–8 % and NVIS +3.7 %.

On air (gateway ⇄ estacao2, 7.050 MHz, against trunk de900c3, interleaved):
- without the round doubling:
  - 8 KB both ways at 20 %: calls of 375/315 s against 350/319 s;
  - 2 KB at 3 %: 709/647/787 s against 714/728/685 s;
- with it, 8 KB both ways at 20 %: 330/307 s against 307/353 s;
- every file intact; the gateway's modem at 8–10 % of a core either way.

In short, a tie.  UUCP's turns keep most rounds at 1–4 frames, and the
station side sits near DATAC17's threshold (about 6.5 dB), where rounds
still grow by one, so the gains the sim shows on long transfers and on
lossy or NVIS channels have little to act on here.  The confirmation fired twice at 20 %: once QAM16C2
delivered 2/2, once it lost both and was declared dead.  Its effect on air is
not shown either way.

Finding: on this link QAM16C2 frames read 4–5 dB below DATAC17 frames
(10–13 against 15–17 dB).  A reading of 9.9 dB on QAM16C2 shut the rung by
its own gate.  The per-mode lines in `freedv_snr_calib` were fitted over
5..11 dB, and DATAC17's slope (1.6) likely over-reads above that.  The
calibration needs points above 12 dB.
