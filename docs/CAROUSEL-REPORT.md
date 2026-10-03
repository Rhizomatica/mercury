# The carousel data plane with the MFSK floor: report

Merged into `mercuryv2` on 2026-10-03 as 7b068e5 (#322, with #328 merged into it). The CALL marker for older callers came after, on branch `carousel-old-callers`. The design and wire formats are in [CAROUSEL-ARQ.md](CAROUSEL-ARQ.md). This report covers what changed, what it was measured against, what broke along the way, and what is still open.

## What changed

Connected sessions used to move data with a stop-and-wait sub-FSM: one frame, one ACK, a turn negotiation, and an SNR-driven gear shift. That is now a receiver-driven erasure-coded carousel:

- **Coding.** Data is cut into blocks of up to 96 pieces of 24 bytes, Reed-Solomon coded so that any K pieces rebuild a block. The pieces fit every mode, so a block survives any mode change.
- **The receiver drives.** It polls ("N frames in mode M", plus what each block still needs) and runs the link adaptation on what actually arrived: goodput per rung, probes, dead verdicts. The sender keys only when asked, sending a round of N bursts in one keydown.
- **The ladder.** MFSK, DATAC15, DATAC4, DATAC3, DATAC1, DATAC17, QAM16C2.
- **The floor.** At MFSK the receiver answers each 13.5 s frame with a 0.64 s pattern (ACK: keep going; BREAK: block done) instead of a DATAC16 poll. CALL and ACCEPT fall back to MFSK when DATAC16 does not get through, so a session can open there too. That puts the bottom of the link about 5 dB lower than before.
- **Control on the floor.** Each end reports in every frame whether it hears the other below DATAC16. To a peer that says so, control goes on MFSK.
- **Carrier sense.** A caught preamble holds the channel for its whole frame, and MFSK carrier sense searches incrementally for a burst that is still arriving.

## Results

### On air

The bench is the gateway (sBitx), estacao2 (sBitx) and estacao8 (IC-7100), on 7.050 MHz into dummy loads. The gateway's TX power sets the SNR; 2 % is about −10 dB at estacao2. Times run from the start of the application's call to the file's arrival, so the connect and the application handshake are included. Every file arrived intact (md5).

**Against stop-and-wait** (same binary, `MERCURY_CAROUSEL=1|0`, runs interleaved):

| when | application, load | carousel | stop-and-wait |
|---|---|---|---|
| 2026-09-24 | NNCP, 2 KB each way, both done | 185 / 185 s | 274 / 291 s |
| 2026-09-24 | NNCP, 8 KB each way, both done | 341 / 361 s | 549 / 565 s |
| 2026-09-28 | UUCP, 8 KB each way, whole call | 346 / 344 s | 497 / 589 s |

**The floor against trunk** (2026-09-29, car12 vs trunk of the day, gateway → estacao2, UUCP):

| gateway power | load | carousel + floor | trunk |
|---|---|---|---|
| 20 % | 10 KB | 188 / 205 s | 298 / 347 s |
| 10 % | 10 KB | 541 / 519 s | 1140 / 1084 s |
| 3 % | 2 KB | 616 / 625 s | 1179 / 1119 s |
| 2 % | 2 KB | 932 / 633 s | 0 of 2: never connected |

**The merged build** (car30–32 and trunk 7b068e5, 2026-10-01 to 10-03). No transmissions overlapped in any of these runs:

| run | time | modes |
|---|---|---|
| 50 KB gateway → IC-7100 | 408 s | QAM16C2 throughout |
| 50 KB IC-7100 → gateway | 641 s | DATAC1, DATAC17 |
| 10 KB gateway ↔ IC-7100 | 163–256 s / 221–226 s | |
| 2 KB gateway → estacao2 at 2 %, five runs | 670–721 s | MFSK; 68–80 % of rounds answered by a pattern |
| 8 KB each way at 20 %, one call | 142 s / 265 s, call 301 s | DATAC1–QAM16C2 |
| 2 KB each way at 2 %, one call | 702 s / 853 s, call 912 s | MFSK one way, DATAC17 the other |

### Simulator

`carousel_bench` with realistic carrier sense (`CAR_CS_DECODABLE`: a frame is sensed only if the listener was bound to its mode when the frame began), 20 seeds per cell, 8 KB, mean time / collisions:

| channel | one way | both ways |
|---|---|---|
| AWGN, 25 % loss | 138 s / 0 | 263 s / 0 |
| cliff 0 dB | 421 s / 0 | 954 s / 0 |
| fade 3 dB, 0.5 Hz | 576 s / 0 | 1261 s / 0 |
| fade −5 dB, 0.5 Hz | 2614 s / 4 | 5711 s / 16 |
| fade −9 dB, 0.5 Hz | 2309 s / 3 | 5305 s / 0 |
| NVIS | 2291 s / 0 | 4130 s / 2 |
| asymmetric −6 / 14 dB | 2389 s / 0 | 2498 s / 0 |

Every run completes and delivers exactly what was sent. In `ab_bench`, which runs both planes through the real FSM, stop-and-wait finished 0 of 20 runs on NVIS and on fade 3 dB, where the carousel finished all of them.

### Real modems through `ch`

8 KB one way, carousel vs stop-and-wait:

| channel | carousel | stop-and-wait |
|---|---|---|
| clean | 53 s | 88 s |
| AWGN 10 dB | 118 s | 150 s |
| AWGN 4 dB | 135 s | 220 s |
| ITU moderate 12 dB | 126 s | 149 s |

At −9 dB the floor delivered 432 B in about 90 s. With DATAC16 unusable both ways (No = −1), the session opened through MFSK CALL/ACCEPT and delivered 216 B without decoding a single DATAC16 frame.

## Compatibility

A carousel ACCEPT is marked (framer extension bit 4), and a carousel CALL is marked in the unused tail of its callsign slot. Both are invisible to older decoders. A station picks the plane per session:

| caller | callee | session |
|---|---|---|
| carousel | carousel | carousel |
| carousel | 1.9.x / `MERCURY_CAROUSEL=0` | stop-and-wait |
| 1.9.x / `MERCURY_CAROUSEL=0` | carousel | stop-and-wait |

Tested in the Go harness with real older binaries (`MERCURY_TEST_BIN_A/_B`): all seven pairings deliver. Also on air, 2026-10-03, 10 KB between the IC-7100 and the gateway, every file intact, no overlapping keydowns:

| IC-7100 | gateway | direction | session | time |
|---|---|---|---|---|
| this build | this build | gateway → IC-7100 | carousel, QAM16C2 | 144 s |
| this build | this build | IC-7100 → gateway | carousel, DATAC1 / DATAC17 | 248 s |
| this build | trunk before the carousel | IC-7100 → gateway | stop-and-wait | 336 s |
| this build | trunk before the carousel | gateway → IC-7100 | stop-and-wait (before the CALL marker: no answer) | 256 s |

Two cases still fail:
- **Pre-release carousel builds**, from before the ACCEPT marker. A new caller falls back while the old callee waits for the carousel, so nothing moves. Released builds are unaffected; update both ends.
- **A callsign whose code fills the whole slot** (about 13–14 characters) cannot carry the CALL marker, so its sessions run stop-and-wait.

## What broke on the way, and why

Most of these were found on air or with realistic sensing in the simulator, not by the unit tests. Grouped by kind:

**Collisions (both stations keyed at once):**
- The receiver re-polled on silence, but a round in a fade is on the air without being sensed; it re-polled into it.
- The floor window raced the sender's wait.
- The 90 s silence nudge met the peer's own timers.
- A deep-floor handover collided with the peer's round (187 → 0 per 20 runs).
- A one-way floor poll was missed while the sender waited.

**Carrier sense:**
- The MFSK decoder only searched once a whole 13.5 s burst was buffered, so carrier sense was dark for the first 13 s of every MFSK frame.
- A frame whose sync dropped mid-frame looked like silence; on air the receiver keyed 2.7 s into a 7.4 s DATAC17 frame. A caught preamble now holds the channel for the frame.

**Different radios:**
- The pattern detector tolerated ±15 Hz, and the IC-7100 and sBitx are further apart than that. Patterns went 0/5 on air before the fix and 20/20 after.

**TX:**
- The QAM16C2 and DATAC17 band-pass filters cut their own outer carriers by 6.5 dB. Fixed on trunk too (#337).
- MFSK had rectangular symbol edges, measured −27 dB out of band on an Airspy. A raised-cosine crossfade brings that to −71 dB.

**Teardown:**
- Crossed disconnects failed to redial (fixed on trunk too, #338).
- A fixed 30 s drain dropped UUCP's final reply at the floor, where one exchange takes 40–50 s.
- ABORT waited for that drain.

**Connect:**
- The callee answered an MFSK CALL only from LISTENING, not from ACCEPTING.
- uuport hung up before a deep connect landed (Rhizomatica/hermes-net #29 and #30, now 180 s).

**Mixed versions.** Before the ACCEPT marker, every mixed pair "connected" and then moved nothing.

**Smaller:**
- The UI's byte counters stayed at 0 in carousel sessions.
- A failed answer left the session seed set, which dropped broadcast frames until the next call.
- The link adaptation stepped straight down to MFSK at 12 dB.

## Open

**Handover against handover in deep fades.** At fade −5 dB both ways the simulator still shows 16 overlaps in 20 runs, about one per 95-minute session. They cost airtime but never data. They have not been seen on air.

The cause: carrier sense is decoder sync, and in a deep fade neither end syncs on the other's DATAC16. So a station hands over, or repeats a handover, while the other is keyed.

Ten variants were measured against the 22-cell matrix, and none was a clean win:

| idea | result |
|---|---|
| bind a never-polled sender to its announced start rung | 16 → 22 |
| send control on MFSK after a missed handover | 16 → 20; asymmetric −9/3 dB 5 → 95 |
| send control on MFSK after an empty poll window | thousands of overlaps, unfinished runs |
| report "control-deaf" after a missed handover (both ends switch) | 16 → 4, but 12 % slower, and fade −3 dB 7 → 123, NVIS 2 → 73 |
| … gated by SNR, or cleared each turn | trades the same way |
| random 0–3 s or 0–6 s jitter on handover repeats | totals 39 → 46 / 66 |

A real fix most likely needs carrier sense from energy rather than decoder sync. That is a modem-level change, and it is risky on noisy bands; the #311 busy detector is off by default for that reason.

Also open:
- **Carrier sense below −9 dB** is limited by the preamble.
- **The receiver's round size** is recomputed from every frame (seen + left), so only a lost last frame ever counts as loss. Correcting that cost 5–8 % on fading, because the margins are tuned to it; it is a retuning job.

## Reproducing

- **Simulator matrix.** `make -C tests carousel_bench`, then `CAR_CS_DECODABLE=1 CAR_LIMIT_S=28800 tests/carousel_bench <seed> <channel> [bidir]`. Channels: `awgn:L`, `cliff:dB`, `fade:dB:Hz`, `nvis`, `asym:A:B`, `step:A:B:T`. `CAR_TRACE=1` prints every decision.
- **Mixed versions.** `MERCURY_TEST_BIN_A=<build> MERCURY_TEST_BIN_B=<build> go test -run '^TestMercuryARQTransfer$' ./tests/integration`. A wrapper script that exports `MERCURY_CAROUSEL=0` works as a build.
- **On air.** Run `utils/onair_logs.py <sender journal> <receiver journal>` on the two stations' Mercury journals for one transfer. It shows how each round was answered, the rungs used, and any keydowns that overlapped.
