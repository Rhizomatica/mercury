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
Bigger frames carry more per transmission but need a better signal to decode at
all. The UI reports the running mode, its bit rate and its bandwidth, computed
from the modem itself rather than this table.

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
real Mercury modems with `ch` standing in for the propagation path:

```
$ utils/bcast_ota_test.sh 9 1000
mode=9  file=1000 B  air=ch clean (--No -100)
  RESULT: recovered byte-identical as "report.bin" in 2s
```

Measured on that harness, 1 kB file, one variable at a time
(`SNR3k = -No - 14.82`; the harness's noise is AWGN, not fading):

| SNR3k | DATAC3 (mode 1, default) | DATAC17 (mode 9) |
|------:|--------------------------|------------------|
| 15.2 dB | 32 s | 2 s |
| 10.2 dB | 32 s | 2 s |
|  7.2 dB | — | 2 s |
|  5.2 dB | 32 s | fails |
|  2.2 dB | 32 s | — |
|  0.2 dB | 32 s | — |
| -2.8 dB | fails | — |
| -4.8 dB | fails | — |

DATAC3 carries a 1 kB file down to about **0 dB SNR3k** and takes the same 32 s
throughout: below the cliff it fails outright rather than slowing down, which is
what a fountain code over a hard-decision modem does — a frame either decodes or
it does not. DATAC17 is 16x faster but gives out between 7 and 5 dB.

### The small-frame modes are not usable for files

Every frame spends 12 bytes on the header, OTI and tag. The 14-byte modes
(DATAC0, DATAC13, DATAC16) therefore carry **2 bytes of payload per frame** —
86% overhead — and a 1 kB file would need over 500 frames, about 17 minutes of
continuous transmission at DATAC13's 1.98 s per frame.

Measured: DATAC13 did not complete even a 60-byte file within 200 s at
+15.2 dB. The codec itself is fine at that symbol size — it produces valid
frames — the mode is simply the wrong tool for a file. Use DATAC4 (42 bytes per
symbol) or larger; DATAC3 is the sensible robust choice.

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
