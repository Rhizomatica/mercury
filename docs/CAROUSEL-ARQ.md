# The carousel data plane

Mercury's connected sessions move data with an erasure-coded "carousel"
driven by the receiver (`datalink_arq/carousel.[ch]`).  It is the only data
plane: the stop-and-wait data-flow sub-FSM of 1.9.x (described in
[ARQ.md](ARQ.md)) is gone.  CALL and ACCEPT open a session, the ACCEPT is the
carousel's first poll, and a DISCONNECT ends it.
[CAROUSEL-SPEC.md](CAROUSEL-SPEC.md) specifies the protocol state by state,
from the code; this page is the overview.

The connect frames carry a protocol version (1) and the carousel bit in the
framer extension (see Wire formats).  1.9.x stations, and earlier carousel
builds, do not connect to this one in either direction: each drops the
other's CALL and ACCEPT.  A CALL from another version that is addressed to
this station is logged ("from another protocol version") and not answered.

## How it works

**Blocks and pieces.**  The application's bytes are cut into blocks of up to
96 pieces, Reed-Solomon coded over GF(256)
(`datalink_arq/rs_erasure.c`, systematic Cauchy: pieces 0..K-1 are the data,
any K of the pieces rebuild the block).  One piece size serves the whole
session and fits every mode, so a block is never re-encoded when the mode
changes: 23 bytes when the caller's rounds start on the floor (the ACCEPT
names rung 0, or says the callee is control-deaf, so both ends know it), 24
otherwise.  An MFSK frame (98 bytes, 6 of headers) carries 4 pieces of 23
(92 bytes) where it carried 3 of 24 (72); the DATAC rungs carry 4 % less at
23, so a session that starts higher keeps 24.  Up to 8 blocks are open at
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

**Only the receiver keeps a round timer.**  It polls again when the round's
window closes, or when the sender's carrier drops if it heard the carrier.
A round that brought nothing is repeated once unscored, keeping its poll id,
on a rung that has already delivered in the session (the poll may have been
lost); otherwise the silence is scored against the mode.

**Link adaptation**, run by the receiver on what it received: the rung with
the best measured goodput (raw rate times delivered fraction); probes only of
rungs that could beat it; a rung's round is capped at one frame more than it
recently delivered; 4 frames lost in a row is a dead verdict, re-probed after
60 s, doubling to 8 min.  A direction starts on the rung the connect's SNR
supports (`ARQ_SNR_MIN_*`), as a one-frame probe.  The session's bandwidth caps the
ladder in both directions: BW500 uses MFSK, DATAC15, DATAC4 and DATAC3;
BW2300 and BW2750 use every rung (nothing is wider than 2.1 kHz yet).

**The turn.**  The receiver takes it with a HANDOVER poll that announces its
own first round, sent in the same keydown; the peer's control decoder reads
the poll and rebinds its payload decoder in the 300 ms gap.  With both sides
holding data, the receiver takes the turn at the first round after 30 s that
completed a block, and after 45 s at the latest; its polls are trimmed to fit
the 45 s.
Open blocks are only suspended.

**Connect.**  The callee's ACCEPT names the rung the caller's first round
goes out on.  The callee binds its payload decoder to it and keys nothing
until it hears that round -- in ACCEPTING it still transmits only after a
CALL.  The caller treats its first round as an unconfirmed handover: if no
poll comes back, it repeats the round behind a handover poll, which names the
mode.

**Session.**  Carousel frames carry the session in their CRC16: the sender
XORs the CRC with a seed (a CRC16 of the session id, the callee's nonce from
the ACCEPT and both callsigns), and every decoder of the station checks
against it.  Another session's frames fail like any bad frame.  The control
decoder also admits plain frames (CALL, ACCEPT, DISCONNECT, broadcast).  HARQ
combining is off in a carousel session: consecutive frames are different
codewords.

**Integrity.**  A CRC16 passes about one corrupt frame in 65536, so every
block also ends in a 4-byte CRC-32 keyed by the seed, checked when the block
decodes.  A block that fails it, or fails to decode, or two frames that
disagree on a block's size, end the session: the stream runs ahead of the
check, so carrying on could deliver wrong data.  At the floor a BREAK pattern
can be another station's, or a detector false alarm, so it only stops the
sender sending a block: only a poll retires one.  And the ACK and BREAK are
the session's own: their tone lists are derived from the seed, so another
station's patterns -- every station once sent the same two -- do not count.

**Carrier sense and the NAV header.**  A station is "on the air" to its
peer when one of the peer's decoders syncs on it.  In a deep fade, or on a
rung the peer's payload decoder is not bound to, neither does, and the peer
keyed over it.  So below 0 dB (as a station hears its peer: the path is
reciprocal) every keydown opens with a NAV header: a 0.64 s pattern whose
tone list says how long the keydown lasts, in 12 classes from 2 s to 32 s.
The peer holds the channel for that long, whether or not it can decode a
frame of it; patterns are detected down to about −14 dB SNR3k on AWGN, where
DATAC16 frames stop near −10 dB (`utils/pattern_probe`).  The detector
(one FFT per 5 ms step, all lists scored by lookup) runs throughout a
session, for about 1 % of an x86 core.  `MERCURY_NAV=0` turns headers off.

## Wire formats

DATA, in a payload mode, filling the frame:

| bytes | field |
|---|---|
| 0 | frames left in the round (4 bits), data not yet cut into blocks (1), the start rung for the peer (3) |
| 1 | poll id (4), highest block opened, mod 16 (4) |
| then per block | 4 bytes: block id mod 16 (4), K-1 (7), piece count (6), first piece index (8), pad bytes in the last data piece (5); then the pieces, consecutive indices mod 256 |

A block is the application's bytes and then its 4-byte check: CRC-32 of the
seed (4 bytes), the block's id and K, and those bytes.

POLL / HANDOVER / STATUS, in DATAC16 (14 bytes):

| byte | field |
|---|---|
| 0 | type (2), has data (1), poll id (4), control-deaf (1) |
| 1 | mode rung (3), frames asked for (4) |
| 2 | loss (4), window base mod 16 (4) |
| 3-9 | pieces each block from the base still needs, 8 x 7 bits (127: none seen) |
| 10 | the first data piece missing from the base block (255: none) |
| 11 | start rung for the peer (3), handover round's rung (3) |
| 12 | handover round's frames (4), its id (4) |
| 13 | highest block opened (4), data not yet cut (1) |

CALL and ACCEPT, protocol version 1: 14 bytes (a DATAC16 frame), padded to
the frame on MFSK.

| byte | CALL | ACCEPT |
|---|---|---|
| 0 | framer: type CALL (3 bits); extension: bandwidth token (2), protocol version (2, = 1), carousel (1) | the same |
| 1 | 0, session id (7) | 1, session id (7) |
| 2-3 | CRC16 of the callsign it calls | CRC16 of the caller's callsign |
| 4-5 | the caller's callsign, arithmetic-coded, to byte 13 | CRC16 of the callsign the caller called (the callee's, primary or secondary) |
| 6-7 | | the callee's session nonce |
| 8 | | the caller's start rung (3 bits; 7: the callee hears it below the control mode, so the floor), 5 bits zero |
| 9-13 | | zero |

- A station takes a CALL or ACCEPT only with version 1 and the carousel bit
  set; a 1.9.x station reads that extension as an unknown bandwidth token
  and drops the frame.  The versions do not connect to each other.
- An ACCEPT with a reserved bit set is dropped.  The caller takes an ACCEPT
  only if its session id, its own CRC and the called station's CRC all
  match: 39 bits, and every one of them full.
- The callee answers every repeat of a session's CALL with the same ACCEPT
  (rung, nonce): the caller may have decoded any of them, so they must not
  differ.  Both ends then cut the same pieces (spec R25).
- The callee's callsign is not on the air in full: the caller knows whom it
  called, and the CRC confirms it.

Patterns are 16 MFSK symbols, 0.64 s: 32 tones, each symbol's tone the
list's (8 tones, repeated) plus 13 per symbol, mod 32.  The detector counts
symbols whose expected tone is the peak bin, searching +/-2 bins of offset.

| pattern | tones | detected at |
|---|---|---|
| ACK, BREAK | the session's (`mfsk_session_patterns` of the CRC seed); global ones with no session | 8 of 16 |
| NAV class k | `mfsk_nav_tones[k]`, k = 0..11: the keydown ends within `mfsk_nav_class_ms(k)` of the header's start | 9 of 16 |

Each list agrees with every other in at most 3 symbols (NAV classes with each
other in at most 4) at any time shift and up to 4 bins of frequency shift.  A
strong pattern can still cross-read as another list at a lower score: of
overlapping events only the strongest is reported.  A headed keydown: head
silence, header, 60 ms, then its bursts.

Bursts of a round are 100 ms apart (QAM16C2: 200 ms).  With no gap the
payload decoder misses bursts after the first; the gaps were measured with
`utils/burst_train_probe`.

## Measurements

The sim runs the real FSM and carousel over the same channel models as the
stop-and-wait plane it replaced (`tests/sim/README.md`); these comparisons
were made while both planes were still built in.  Both stations send 8 KB,
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
- `tests/modem/test_pattern_ack_detection`: the streaming detector against
  the batch one (bit-identical), session lists and NAV classes (separation,
  each heard as itself and nothing else); `utils/pattern_probe` (`wcurve`,
  `nav`, `xread`, `PATTERN_KEY`) measures curves, false alarms, cross-reads.
- `tests/test_car_explore`: the carousel under every set of up to 3 lost
  frames and 3 unsensed keydowns, every single spurious BREAK and every
  single corrupt frame; `specs/carousel/` has the TLA+ model of the block
  layer and what both have shown.
- `tests/integration`: two real daemons over FIFO audio.
  `MERCURY_TEST_BIN_A` / `MERCURY_TEST_BIN_B` run one station on another
  build, to compare builds of this protocol version:
  `MERCURY_TEST_BIN_B=/path/to/mercury go test -run '^TestMercuryARQTransfer$' .`
- `tests/datalink_arq/test_arq_protocol`: `test_connect_v1_call`,
  `test_connect_v1_accept`, `test_connect_other_version_refused`.
- `utils/onair_logs.py`: one on-air run from the two stations' journals --
  how rounds were answered, the rungs used, and any overlapping keydowns.
- `tests/carousel_bench` (the carousel alone), `tests/ab_bench` (under the
  full FSM): the throughput matrix.
