# Broadcast file transfer

Sending a file over HF to whoever happens to be listening, with no
acknowledgement and no return path.

This is a different problem from ARQ. There is no negotiation, no
retransmission request, and no way to know who received anything — a station
may start listening in the middle of a transmission, lose arbitrary stretches
of it, and still has to end up with the file. Mercury solves it the way
[hermes-broadcast](https://github.com/Rhizomatica/hermes-broadcast) does, with a
RaptorQ fountain carousel, and is **wire-compatible with that project's
`broadcast_daemon`**: a Mercury station and a hermes-broadcast station
interoperate in both directions.

The byte-exact protocol is specified in
[BROADCAST-WIRE-FORMAT.md](BROADCAST-WIRE-FORMAT.md); this page is the
operator-facing description.

## How it works

The file is encoded into a stream of RaptorQ symbols and transmitted round and
round. A receiver collects symbols — any symbols, it does not matter which —
and decodes as soon as it has slightly more than the file's worth. Loss costs
time, not correctness.

Every frame is self-describing:

```
 byte 0      packet type (3 bits) | session id (5 bits)
 bytes 1-8   reduced OTI: 24-bit file length, 16-bit symbol size,
             Z-1, 16-bit N-1
 bytes 9-11  reduced tag: source block number, 16-bit encoding symbol id
 bytes 12+   the symbol
```

Carrying the transfer parameters in *every* frame costs 8 bytes per frame and
is what lets a receiver with no return path start decoding on the first frame
it hears, instead of waiting for a periodic configuration frame to come round.
The session id distinguishes one file from the next, so a receiver restarts
cleanly rather than mixing symbols from two files into a decode that would
never converge.

### The bundle

RaptorQ moves an opaque blob, so the filename travels inside it, in
mercury-connector's `spool.c` layout:

```
 bytes 0-3   uint32 little-endian: length of everything after this field
 bytes 4..   the basename, terminated by '\n'
 then        the file
```

Only the basename is ever sent. A bundle arriving off the air is unauthenticated
input from an unknown station, so the parser refuses anything containing a path
separator or NUL, refuses `.` and `..`, and refuses a length field that
disagrees with the buffer. A receiver must not be talked into writing outside
its own download directory by whatever the sender put in the name field.

### One source block

RaptorQ can partition a file into several source blocks, each coded
independently. Mercury uses **one** wherever it fits, because the alternative
costs twice over:

* the fountain's overhead is paid per block, so nanorq's default of 16 blocks
  needs 5–17% more frames than a single block for the same file;
* a loss pattern that happens to be periodic in the carousel starves one block
  completely — measured at 0 symbols out of 15000 dropped, never decoding.

A block holds at most 56403 symbols, so a small symbol size on a large file
still needs more than one; the smallest legal number is used, never the
default 16.

## Limits

| | |
|---|---|
| Maximum file | 1.44 MB, counting the bundle (file + name + header) |
| Minimum mode | anything with a frame larger than 12 bytes — DATAC14 (3 bytes) cannot carry broadcast |
| ESI range | 16 bits, so the carousel ends after 65535 symbols per block |

The size cap is a floppy disk, and it is deliberate: the file is held in memory,
and a carousel over HF is slow enough that a much larger file is a mistake worth
refusing up front rather than discovering hours in. At DATAC3 — the default
mode — 1 kB takes about half a minute.

## Choosing the mode

**Both stations must be set to the same mode.** There is no negotiation on the
broadcast plane, and no runtime mode switch: the mode is fixed when Mercury
starts.

```
mercury -m <index>          # see `mercury -l` for the list; default is 1 (DATAC3)
```

| index | mode | payload | bit/s | bandwidth | |
|-------|---------|--------:|------:|----------:|---|
| 0 | DATAC1  |  510 B |  976 | 1687 Hz | |
| 1 | DATAC3  |  126 B |  316 |  562 Hz | default |
| 2 | DATAC0  |   14 B |  255 |  562 Hz | |
| 3 | DATAC4  |   54 B |   84 |  250 Hz | |
| 4 | DATAC13 |   14 B |   57 |  187 Hz | |
| 5 | DATAC14 |    3 B |   52 |  222 Hz | *too small for broadcast* |
| 6 | FSK_LDPC|   30 B |   44 |  n/a    | not OFDM |
| 7 | DATAC15 |   30 B |   64 |  187 Hz | |
| 8 | DATAC16 |   14 B |   36 |  187 Hz | |
| 9 | DATAC17 | 1180 B | 1407 | 2062 Hz | |
| 10| QAM16C2 | 1213 B | 3130 | 2062 Hz | |

All figures measured from the modems themselves, not quoted from a datasheet.
The **Benchmarks** section below gives each mode's measured SNR floor and how
long a 5 kB file actually takes on it.

Bigger frames carry more per transmission but need a better signal to decode at
all. The UI reports the running mode, its bit rate and its bandwidth, computed
from the modem itself rather than this table.

## Talking to Mercury's broadcast port

Anything sending modem frames — a file transmitter, a relay — must use KISS
command `0x03` (`CMD_MODEM_FRAME`), which tells Mercury the payload is already
a modem frame and must go out untouched. Any other command means "a message",
and Mercury adds a header and length prefix, costing 3 bytes and corrupting a
frame that was already the right size.

Mercury does not guess from the payload's contents. See
[BROADCAST-WIRE-FORMAT.md](BROADCAST-WIRE-FORMAT.md) §1.

## Using it

### From the UI

*Launch Mercury Client* → *Connect modem*, then the **Broadcast file** panel:

* **Send** — choose a file, pick how many carousel cycles to run (or *Until
  stopped*), press *Broadcast*. *Stop* ends it at the next frame boundary.
* **Receive** — tick *Receive broadcast files* and choose a folder. Both the
  folder and the tick are remembered, so a station configured once comes back up
  receiving after a restart with nobody present.

File transfer shares the broadcast socket with broadcast chat: Mercury's
broadcast port accepts exactly one client, so the panel filters incoming frames
and passes anything that is not a file frame through to chat.

Progress is reported as frames and cycles, never a percentage. With no return
path the sender cannot know what any receiver has decoded, and a percentage
would be a guess about someone else's decoder presented as a fact.

### From the command line

`utils/bcast_file_tool` does the same thing without the UI (Linux and macOS;
on Windows use the UI, which runs the same code in-process):

```
bcast_file_tool send <file> [-m mode] [-c cycles] [-i ip] [-p port]
bcast_file_tool recv <dir>  [-m mode] [-i ip] [-p port]
```

`-c 0` (the default) repeats until interrupted.

## Testing

`utils/bcast_ota_test.sh <mode> <bytes> [No]` runs a whole transfer between two
real Mercury modems with `ch` standing in for the propagation path. It is what
produced the benchmarks below:

```
$ utils/bcast_ota_test.sh 9 1000
mode=9  file=1000 B  air=ch clean (--No -100)
  RESULT: recovered byte-identical as "report.bin" in 2s
```

## Benchmarks

A 5 kB file, transmitted between two real Mercury modems with `ch` standing in
for the propagation path. Each mode was walked down in SNR until it stopped
decoding; `SNR3k = -No - 14.82`, and the harness's noise is **AWGN, not
fading** — a real HF path will be worse.

### What each mode costs and buys

| mode | | frame | 5 kB in | goodput | works down to | fails at |
|------|--:|------:|--------:|--------:|--------------:|---------:|
| QAM16C2 | 10 | 1213 B | **17 s** | 2353 bps | +17.2 dB | +15.2 dB |
| DATAC17 | 9 | 1180 B | 31 s | 1290 bps | +7.2 dB | +6.2 dB |
| DATAC1 | 0 | 510 B | 50 s | 800 bps | +5.2 dB | +3.2 dB |
| DATAC3 | 1 | 126 B | 167 s | 240 bps | +0.2 dB | −1.8 dB |
| DATAC4 | 3 | 54 B | 695 s | 58 bps | −6.8 dB | −7.8 dB |
| MFSK | 11 | 98 B | 786 s | 51 bps | **−9.8 dB** | −12.8 dB |

The span is the whole point: **QAM16C2 moves the file 46× faster than MFSK, and
MFSK works 27 dB further down.** Pick for the path you have, not the one you
want — and remember both stations must be set to the same mode by hand.

MFSK (mode 11) is the robust end of the ladder, 3 dB below DATAC4 while
carrying a *larger* frame (98 B against 54 B) — it buys that margin with time
per frame (12.8 s) rather than with payload, which is why its 5 kB time is only
13% worse than DATAC4's for 3 dB more reach. It is Mercury's own modem rather
than a FreeDV mode; both stations need `mercury -m 11`.

### Every point measured

```
QAM16C2  +20.2 ok 17s    +17.2 ok 17s    +15.2 FAIL     +12.2 FAIL
DATAC17  +20.2 ok 31s    +15.2 ok 31s    +10.2 ok 31s    +7.2 ok 31s
          +6.2 FAIL       +5.2 FAIL       +2.2 FAIL
DATAC1   +15.2 ok 50s    +10.2 ok 50s     +5.2 ok 50s    +3.2 FAIL
          +2.2 FAIL       +0.2 FAIL
DATAC3   +10.2 ok 167s    +5.2 ok 167s    +2.2 ok 167s   +0.2 ok 167s
          -1.8 FAIL       -2.8 FAIL       -4.8 FAIL
DATAC4    +5.2 ok 695s    +0.2 ok 695s    -2.8 ok 695s   -4.8 ok 701s
          -6.8 ok 2110s   -7.8 FAIL       -8.8 FAIL
MFSK     clean ok 786s    -9.8 ok 786s   -12.8 FAIL     -14.8 FAIL
         -17.8 FAIL      -19.8 FAIL
```

The MFSK points were run with a 1800 s cap — 2.3 carousel passes against a
786 s flat time. That is enough to separate "decodes" from "does not", but it
would not have caught a DATAC4-style degradation zone, where the last working
point took 3× the flat time. So −12.8 dB is where MFSK stops decoding *within
2.3 passes*; a station willing to leave a much longer transfer running may get
a little further down. The −9.8 dB figure in the table is a floor that is
safe to plan against, not the theoretical limit.

### How the failure behaves

**Time is flat with SNR, then the mode stops working.** DATAC3 takes the same
167 s at +10 dB as at +0.2 dB. That is the fountain code doing its job: the
carousel sends a fixed number of symbols per pass, and as long as enough of them
decode the file completes in one pass regardless of how much margin is left.

**The cliff is sharp — 1 to 2 dB in every case.** There is no useful "slow but
working" region to operate in, so a mode chosen 3 dB above its floor is not
meaningfully more reliable than one chosen 1 dB above it; it is either decoding
or it is not.

**With one exception, at the very edge.** DATAC4's last working point took
2110 s against 695 s everywhere above it — 3× — because enough symbols were
being lost that the carousel had to go round repeatedly. So there *is* a
degradation zone, but it is narrow (roughly the last 2 dB) and it costs time
rather than correctness. If a transfer is taking several times longer than the
table says, the link is at that mode's edge and the next mode down is the
answer.

### The small-frame modes are not usable for files

Every frame spends 12 bytes on the header, OTI and tag. The 14-byte modes
(DATAC0, DATAC13, DATAC16) therefore carry **2 bytes of payload per frame** —
86% overhead — and a 1 kB file would need over 500 frames, about 17 minutes of
continuous transmission at DATAC13's 1.98 s per frame.

Measured: DATAC13 did not complete even a 60-byte file within 200 s at
+15.2 dB -- which is why no 14-byte mode appears in the table above. The codec itself is fine at that symbol size — it produces valid
frames — the mode is simply the wrong tool for a file. Use DATAC4 (42 bytes per
symbol) or larger; DATAC3 is the sensible robust choice, and MFSK (86 bytes per
symbol) is the choice when the path will not carry anything else.

Two things the harness encodes, both learned the hard way: `cat` between the
FIFOs does **not** work as the air — it buffers in 64 kB chunks, so the
receiving modem never syncs — and no sender pacing is needed, because
`write_buffer()` blocks when the ring is full and TCP backpressure already
paces it.

## Interoperating with hermes-broadcast

All three combinations are tested through two real Mercury modems:

| sender | receiver | result |
|---|---|---|
| Mercury | Mercury | file recovered under its original name |
| hermes-broadcast `transmitter` | hermes-broadcast `receiver` | works — 1 kB at DATAC3 in 67 s |
| hermes-broadcast `broadcast_daemon` | Mercury | file recovered, byte-identical |

**hermes-broadcast still works standalone through Mercury.** `transmitter` and
`receiver` use the older split format (a separate periodic configuration frame,
extension field zero) and are unaffected by anything here.

`broadcast_daemon` uses the same joint frame Mercury does, so the two
interoperate. It transmits the bare file rather than a bundle — the bundle is
Mercury's convention for carrying a filename, not part of RaptorQ — so Mercury
saves what it receives as `broadcast_<timestamp>.bin`, the same convention the
daemon uses in the other direction. A decode that succeeded is never discarded
for lacking the wrapper.

In the other direction, a daemon receiving from Mercury writes
`broadcast_<timestamp>.bin` whose contents are the bundle; the original name is
inside it.

Note that `transmitter`/`receiver` and `broadcast_daemon` do not interoperate
with *each other* — that predates this work and is a property of the two frame
formats.
