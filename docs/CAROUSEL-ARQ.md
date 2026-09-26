# The carousel data plane

Mercury's connected sessions move data with an erasure-coded "carousel"
driven by the receiver (`datalink_arq/carousel.[ch]`).  It replaces the
stop-and-wait data-flow sub-FSM described in [ARQ.md](ARQ.md); CALL, ACCEPT
and DISCONNECT are unchanged but for the ACCEPT, which is the carousel's first
poll.  There is no on-air compatibility with 1.9.x or with earlier 2.0
builds: both ends of a session must run a carousel build.

`MERCURY_CAROUSEL=0` in the environment runs the stop-and-wait plane instead.
It is kept only to measure one against the other on air, and will be removed
once the carousel is validated there.

## How it works

**Blocks and pieces.**  The application's bytes are cut into blocks of up to
96 pieces of 24 bytes, Reed-Solomon coded over GF(256)
(`datalink_arq/rs_erasure.c`, systematic Cauchy: pieces 0..K-1 are the data,
any K of the pieces rebuild the block).  24-byte pieces fit every mode, so a
block is never re-encoded when the mode changes.  Up to 8 blocks are open at
once.

**The receiver drives.**  The modem runs a control decoder (DATAC16) and one
payload decoder bound to the mode it expects; nothing on the air says which
mode a burst is in.  So the receiver chooses the mode, and it is also the
side that sees what arrives.  Per direction:

- the receiver sends a POLL in DATAC16: the window base, what each open block
  still needs, and "send me up to N frames in mode M";
- the sender answers with a round -- one keydown of up to N separate bursts,
  each its own preamble, frame and postamble -- and keys only when polled;
- every frame says how many follow, so the receiver knows when the round ends
  and polls again.  No sequence numbers, no per-frame ACKs.

**Only the receiver keeps a round timer.**  If the sender is not on the air
by the time it should be, the poll was lost: poll again at once, and do not
score the mode for it.  If it is on the air but nothing decodes, poll when its
carrier drops.

**Link adaptation**, run by the receiver on what it received: the rung with
the best measured goodput (raw rate times delivered fraction); probes only of
rungs that could beat it; a rung's round is capped at one frame more than it
recently delivered; 4 frames lost in a row is a dead verdict, re-probed after
60 s, doubling to 8 min.  A direction starts on the rung the connect's SNR
supports (the thresholds the stop-and-wait plane uses), as a one-frame probe.

**The turn.**  The receiver takes it with a HANDOVER poll that announces its
own first round, sent in the same keydown; the peer's control decoder reads
the poll and rebinds its payload decoder in the 300 ms gap.  With both sides
holding data, a turn runs at most 120 s.  Open blocks are only suspended.

**Connect.**  The callee's ACCEPT names the rung the caller's first round
goes out on.  The callee binds its payload decoder to it and keys nothing
until it hears that round -- in ACCEPTING it still transmits only after a
CALL.  The caller treats its first round as an unconfirmed handover: if no
poll comes back, it repeats the round behind a handover poll, which names the
mode.

**Session.**  Carousel frames carry the session in their CRC16: the sender
XORs the CRC with a seed (a CRC16 of the session id and both callsigns), and
every decoder of the station checks against it.  Another session's frames
fail like any bad frame.  The control decoder also admits plain frames
(CALL, ACCEPT, DISCONNECT, broadcast).  HARQ combining is off in a carousel
session: consecutive frames are different codewords.

## Wire formats

DATA, in a payload mode, filling the frame:

| bytes | field |
|---|---|
| 0 | frames left in the round (4 bits), data not yet cut into blocks (1), the start rung for the peer (3) |
| 1 | poll id (4), highest block opened, mod 16 (4) |
| then per block | 4 bytes: block id mod 16 (4), K-1 (7), piece count (6), first piece index (8), pad bytes in the last data piece (5); then the pieces, consecutive indices mod 256 |

POLL / HANDOVER / STATUS, in DATAC16 (14 bytes):

| byte | field |
|---|---|
| 0 | type (2), has data (1), poll id (4) |
| 1 | mode rung (3), frames asked for (4) |
| 2 | loss (4), window base mod 16 (4) |
| 3-10 | pieces each block from the base still needs (255: none seen) |
| 11 | start rung for the peer (3), handover round's rung (3) |
| 12 | handover round's frames (4), its id (4) |
| 13 | highest block opened (4), data not yet cut (1) |

The ACCEPT carries the caller's start rung in the top 3 bits of its DST CRC,
which an ACCEPT checks on 13 bits alongside its 7-bit session id.

Bursts of a round are 100 ms apart (QAM16C2: 200 ms).  With no gap the
payload decoder misses bursts after the first; the gaps were measured with
`utils/burst_train_probe`.

## Measurements

The sim runs the real FSM and carousel over the same channel models as the
stop-and-wait plane (`tests/sim/README.md`).  Both stations send 8 KB,
carrier sense on, seeds 1-20 / 21-40:

| channel | stop-and-wait | carousel |
|---|---|---|
| clean | 231 / 234 s | 128 / 126 s |
| 10 % loss | 275 / 294 s | 142 / 153 s |
| 25 % loss | 416 / 450 s | 203 / 217 s |
| cliff 3 dB | 1373 / 1380 s | 1018 / 1020 s |
| cliff 10 dB | 310 / 309 s | 251 / 248 s |
| NVIS | 0/20 complete | 3872 / 4091 s |
| fade 3 dB, 0.5 Hz | 0/20 complete | 1299 / 1324 s |
| fade 8 dB, 1 Hz | 750 / 721 s (18/20) | 538 / 504 s |
| fade 15 dB, 1 Hz | 243 / 241 s | 209 / 210 s |
| fade 25 dB, 1 Hz | 179 / 182 s | 110 / 108 s |

Every carousel run completes, with no collisions and no corruption; the
stop-and-wait plane has 35-164 collisions per 20 runs.

On the real modem -- two daemons over FIFO audio through codec2's `ch`
(`tests/integration`, `TestMercuryARQTransfer`, 8 KB one way, including the
connect; one run each, so indicative):

| channel | stop-and-wait | carousel |
|---|---|---|
| clean | 88 s | 53 s |
| AWGN 10 dB | 150 s | 118 s |
| AWGN 4 dB | 220 s | 135 s |
| ITU moderate fading, 12 dB | 149 s | 126 s |

## Tests

- `tests/test_carousel`: the carousel alone, 150 sessions over 10 channels --
  every session completes, delivers exactly what was sent, never keys over
  the peer.
- `tests/test_carousel_sim`: through the real FSM on the two-FSM sim --
  connect, transfers, loss, a peer vanishing, disconnect, and a fuzz.
- `tests/test_crc_seed`: the seeded CRC in freedv.
- `tests/integration`: two real daemons over FIFO audio; the suite runs on
  the carousel.
- `tests/carousel_bench`, `tests/ab_bench` (`CAROUSEL=0` for the old plane):
  the throughput matrix.
