# HERMES broadcast wire format

Byte-exact specification of the RaptorQ file broadcast carried over Mercury's
broadcast plane. Implemented by
[hermes-broadcast](https://github.com/Rhizomatica/hermes-broadcast) and by
Mercury (`datalink_broadcast/bcast_file.c`); this document is kept identical in
both repositories.

Everything here is **wire format**. Changing any of it breaks interoperability
with deployed stations.

---

## 1. Transport

A client connects to Mercury's broadcast TCP port (default 8100) and exchanges
**KISS**-framed packets. Mercury accepts exactly **one** broadcast client at a
time.

```
FEND (0xC0) | cmd | payload (escaped) | FEND (0xC0)
```

Escaping: `0xC0 -> 0xDB 0xDC`, `0xDB -> 0xDB 0xDD`.

The KISS payload is **one modem frame**, exactly
`payload_bytes_per_modem_frame` long for the configured mode.

`cmd` MUST be `0x03` (`CMD_MODEM_FRAME`): *this payload is already one modem
frame, transmit it untouched*.

| cmd | meaning |
|-----|---------|
| `0x00` `CMD_AX25`, `0x01` `CMD_AX25CALLSIGN` | a message; Mercury frames it for the air |
| `0x02` `CMD_DATA` | a message; Mercury frames it for the air |
| `0x03` `CMD_MODEM_FRAME` | a modem frame; transmitted untouched |

"Framing for the air" means Mercury prepends its own 1-byte header and a 2-byte
length prefix, so the receiver can recover the message's exact length from a
frame the modem has zero-padded. It costs 3 bytes, which is why a payload that
already fills the frame is truncated by 3 to make room — harmless for a message,
fatal for a modem frame.

**A sender must therefore declare which it has.** Mercury does not inspect the
payload to decide. It used to: `CMD_DATA` was passed through untouched when its
first byte looked like a broadcast packet type. That could not be made correct,
because those are bytes the sender chooses — a message beginning `0x60..0x9F`
(backtick and every lowercase letter) was passed through unframed, and a real
modem frame sent under any other command was quietly truncated.

`CMD_AX25` and `CMD_AX25CALLSIGN` are unaffected and always framed, which is
what VARA-compatible clients depend on.

> **Upgrade note.** A sender that transmits modem frames as `CMD_DATA` must be
> updated to `CMD_MODEM_FRAME`; against a current Mercury its frames would
> otherwise be framed as messages and truncated. This affects
> hermes-broadcast's `transmitter`, `receiver` and `broadcast_daemon`, all of
> which now send `0x03`. Update both ends together.

## 2. Frame header byte

Every modem frame begins with one header byte:

```
 bit   7   6   5   4   3   2   1   0
     +---+---+---+---+---+---+---+---+
     |  packet type  |   extension   |
     +---+---+---+---+---+---+---+---+
        (3 bits)         (5 bits)
```

```
packet_type = (header >> 5) & 0x07
extension   =  header       & 0x1F
```

Packet types used by this protocol:

| value | name | meaning |
|-------|------|---------|
| `0x03` | `PACKET_RQ_CONFIG` | joint config+payload frame (§3); also the legacy config packet (§4) |
| `0x04` | `PACKET_RQ_PAYLOAD` | legacy payload frame (§4) |

Mercury's own framer calls `0x03` `PACKET_TYPE_BROADCAST_CONTROL` and `0x04`
`PACKET_TYPE_BROADCAST_DATA`. The values are the same; the names differ.

## 3. Joint frame (current format)

Used by `broadcast_daemon` and by Mercury. **Every frame is self-describing**,
so a receiver with no return path can begin decoding on the first frame it
hears rather than waiting for a periodic configuration frame.

```
 offset  size  field
 ------  ----  -----------------------------------------------------------
   0       1   header: packet type 0x03, extension = session id (1..31)
   1       8   configuration body (§5)
   9       3   RaptorQ tag (§6)
  12     rest  encoding symbol
```

Overhead is **12 bytes per frame**. Symbol size `T = frame_size - 12`.

The **session id** is a per-file value in 1..31, chosen at random by the sender
and constant for that file. `0` means "no session". A receiver that sees a
different session id must discard its partial decode and start again: symbols
from two different files never combine into anything.

## 4. Split frame (legacy format)

Used by hermes-broadcast's `transmitter` and `receiver`. Kept for compatibility;
new implementations should use §3.

Configuration packet, sent periodically, zero-padded to the full frame size:

```
 offset  size  field
   0       1   header: packet type 0x03, extension 0
   1       8   configuration body (§5)
   9    rest   zero padding
```

Payload frame:

```
 offset  size  field
   0       1   header: packet type 0x04, extension 0
   1       3   RaptorQ tag (§6)
   4    rest   encoding symbol
```

Overhead is 4 bytes per payload frame, but a receiver cannot decode anything
until a configuration packet arrives, and one whole frame per cycle is spent on
it. The two formats do **not** interoperate with each other.

## 5. Configuration body (8 bytes)

A size-reduced encoding of RFC 6330's Object Transmission Information. The
standard OTI is 12 bytes; this carries the same information in 8, which is what
makes it fit a small HF frame.

```
 offset  size  field
   0       3   F   - transfer length in bytes, little-endian (24 bits)
   3       2   T-1 - symbol size minus one, little-endian (16 bits)
   5       1   Z-1 - number of source blocks minus one (8 bits)
   6       2   N-1 - number of sub-blocks minus one, little-endian (16 bits)
```

`Al` (symbol alignment) is not transmitted; it is always **1**.

A receiver reconstructs nanorq's two OTI words as:

```c
common =  ((uint64_t)b[0] << 24) | ((uint64_t)b[1] << 32) | ((uint64_t)b[2] << 40)
        |  (uint64_t)b[3]        | ((uint64_t)b[4] << 8);

scheme =  ((uint32_t)b[5] << 24) | ((uint32_t)b[6] << 8) | ((uint32_t)b[7] << 16)
        |  1;   /* Al */
```

which are `F<<24 | (T-1)` and `(Z-1)<<24 | (N-1)<<8 | Al` respectively — the
values `nanorq_oti_common()` and `nanorq_oti_scheme_specific()` return.

Because `F` is 24 bits, the largest object this protocol can describe is
16777215 bytes. Implementations impose their own, smaller limits (§8).

## 6. RaptorQ tag (3 bytes)

```
 offset  size  field
   0       1   SBN - source block number
   1       2   ESI - encoding symbol id, little-endian (16 bits)
```

The standard RFC 6330 tag is 4 bytes with a 24-bit ESI. Reducing it to 16 bits
caps the carousel at **65535 symbols per block**, which is far beyond what an HF
link will transmit in practice.

Reconstructed as nanorq expects:

```c
tag = ((uint32_t)t[0] << 24) | (uint32_t)t[1] | ((uint32_t)t[2] << 8);
```

## 7. The bundle (optional payload convention)

RaptorQ transfers an opaque object, so a filename has to travel inside it.
Mercury wraps the file; `broadcast_daemon` does not.

```
 offset  size  field
   0       4   uint32 little-endian: length of everything after this field,
               i.e. strlen(basename) + 1 + file bytes
   4     var   basename, terminated by '\n' (not NUL)
  ...   rest   file contents
```

This layout is mercury-connector's (`spool.c`). Two deliberate refinements: the
length is written **explicitly little-endian** (spool.c writes a native
`uint32`, which is wrong on a big-endian host), and only the **basename** is
ever transmitted.

**A bundle arriving off the air is unauthenticated input.** A conforming
receiver MUST reject a name containing a path separator or NUL, MUST reject
`.` and `..`, MUST reject a length field that disagrees with the object size,
and MUST reject a name with no `'\n'` terminator. Otherwise a sender chooses
where the receiver writes.

A receiver that decodes an object which is **not** a valid bundle MUST still
save it — the decode succeeded, so the data is good. Name it
`broadcast_<YYYYMMDD>_<HHMMSS>.bin`.

### Claiming a frame

A receiver MUST NOT decide a frame is its own from the header byte alone. The
header's top three bits are `3` for any byte in `0x60..0x7F` — backtick and every
lowercase letter — so a broadcast chat line that happens to be exactly one frame
long and begins with a lowercase letter presents a valid-looking packet type and
session id. Claiming it would swallow the message and reset any decode in
progress.

Before accepting a frame, a receiver MUST check that its configuration body
describes a transfer that could be arriving on the configured mode:

* the declared symbol size `T` MUST equal `frame_size - 12`;
* the declared object length `F` MUST be non-zero and within the
  implementation's limit.

Text would have to encode `T-1` in two specific bytes by coincidence and then
pass the length check as well.

## 8. Constraints

| | |
|---|---|
| Minimum frame size | 13 bytes (12 overhead + 1 symbol byte). DATAC14's 3-byte frame cannot carry broadcast. |
| Practical minimum | frames of 30 bytes or more. A 14-byte frame leaves 2 payload bytes — 86% overhead — needing 500+ frames for 1 kB. |
| Maximum object | 16777215 bytes by the 24-bit `F`; Mercury caps at 1.44 MB (the bundle, not the bare file). |
| Symbols per block | 65535 by the 16-bit ESI; RFC 6330 also caps a block at `K_max` = 56403 symbols. |
| Source blocks | Z=1 wherever `ceil(F/T) <= K_max`. More blocks cost the fountain's overhead per block and let periodic loss starve one block entirely. |

## 9. Mode agreement

There is **no negotiation** on the broadcast plane and no runtime mode switch.
Both stations must be started with the same mode (`mercury -m <index>`), and a
receiver silently ignores any frame whose length is not its mode's frame size.

## 10. Interoperability

| sender | receiver | works |
|---|---|---|
| Mercury | Mercury | yes — filename preserved via §7 |
| `broadcast_daemon` | Mercury | yes — saved as `broadcast_<timestamp>.bin` (no bundle) |
| Mercury | `broadcast_daemon` | yes — saved as `broadcast_<timestamp>.bin`, contents are the bundle |
| `transmitter` | `receiver` | yes — legacy split format (§4) |
| `transmitter` | Mercury / daemon | **no** — different frame format |

All rows except the last two were verified between two Mercury modems over a
simulated HF path; the `transmitter`/`receiver` row was verified the same way.
