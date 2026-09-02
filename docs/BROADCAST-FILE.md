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

Measured on that harness:

| mode | file | channel | time |
|------|------|---------|------|
| 9 (DATAC17) | 1 kB | clean | 2 s |
| 9 (DATAC17) | 5 kB | clean | 31 s |
| 0 (DATAC1)  | 5 kB | clean | 50 s |
| 1 (DATAC3)  | 1 kB | clean | 32 s |
| 9 (DATAC17) | 1 kB | SNR3k 10.2 dB | 2 s |

Two things the harness encodes, both learned the hard way: `cat` between the
FIFOs does **not** work as the air — it buffers in 64 kB chunks, so the
receiving modem never syncs — and no sender pacing is needed, because
`write_buffer()` blocks when the ring is full and TCP backpressure already
paces it.

## Interoperating with hermes-broadcast

Mercury emits `broadcast_daemon`'s frame format, so:

* a Mercury station transmitting is received by `broadcast_daemon`, which
  writes what arrives as `broadcast_<timestamp>.bin`. That file is the bundle;
  the original name is inside it. The daemon has no filename mechanism of its
  own.
* a `broadcast_daemon` station transmitting is received by Mercury, which
  recovers the name if the sender bundled one.

`transmitter`/`receiver` in hermes-broadcast use the older split format (a
separate periodic configuration frame) and do **not** interoperate with this.
