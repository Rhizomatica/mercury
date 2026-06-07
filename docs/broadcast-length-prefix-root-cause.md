# Why VarAC broadcasts don't decode without this patch (root cause)

This documents *why* the broadcast length-prefix patch is needed, based on a live
diagnosis between two stations (one Linux/EmComm with VarAC under Wine, one
Windows with VarAC native), both using Mercury 1.9.9 as the HF modem.

## Symptom
Using VarAC's **Broadcast** function over Mercury's broadcast TNC (port 8100):
- A station could **transmit** broadcasts that the other decoded.
- But a freshly (re)connected VarAC **would not receive** any broadcast until it
  **transmitted one itself**; after transmitting once, it then received all
  subsequent broadcasts for the rest of the session.
- When broadcasts *did* show, the message was followed by a run of **squares**
  (boxes) — visible on the Linux/Wine station.

## Root cause — two independent defects in stock Mercury
Both live in the broadcast → client delivery path
(`data_interfaces/tcp_interfaces.c`).

### 1. Format latch → "can't receive until I transmit"
Mercury chooses how to hand a *received* broadcast to the local client from a
flag, `bcast_reply_cmd`:
- It is initialized to `CMD_DATA` (raw) and is **reset to `CMD_DATA` every time a
  broadcast client connects** (`tcp_interfaces.c:68`, `:1126`).
- It only flips to `CMD_AX25CALLSIGN` when the **local client transmits** a
  broadcast (`:869`).

So immediately after VarAC (re)connects, received broadcasts are forwarded as raw
`CMD_DATA`, which a VARA/AX.25 client (VarAC) does not recognize and silently
drops. The first broadcast the operator *sends* latches the flag to AX.25, and
from then on received broadcasts are delivered correctly. That is exactly the
"won't receive until I transmit, then it works" behavior.

Observed in the logs: `kiss_cmd=0x02 payload=126` (raw CMD_DATA, dropped) before
transmitting → `kiss_cmd=0x01 ...` (AX.25, shown) after.

### 2. Zero-padding not trimmed → the "squares"
The broadcast datalink uses a **fixed-size** modem frame (126 bytes for DATAC3).
Stock Mercury delivers `frame_size - HEADER_SIZE` = **125 bytes regardless of the
real message length**, so the short AX.25 frame is followed by ~85 zero bytes.
VarAC renders those trailing nulls as **squares**. (Native Windows VarAC stops at
the first null, hiding them; VarAC under **Wine** draws them as boxes — same bytes
from Mercury, just a rendering difference.)

Observed: `Sending KISS frame to client: kiss_cmd=0x01 payload=125` — always 125,
never the real length.

## What the patch does
In `bcast_process_decoded_frame()` / `bcast_get_tx_payload()`:
1. **TX**: after the 1-byte Mercury header, write a **2-byte big-endian payload
   length**, then the payload, then zero-pad — flagged with a header extension bit
   (`BCAST_EXT_LEN_PREFIX`).
2. **RX**: if the received frame's own header carries that flag, deliver exactly
   the original AX.25 payload as `CMD_AX25CALLSIGN` — **detected from the frame
   itself**, independent of the latched `bcast_reply_cmd`.

This fixes **both** defects at once: padding is trimmed (no squares), and a
receive-only / just-connected station decodes correctly without transmitting
first (no format-latch dependency).

## All-or-nothing: every station must be patched
The patch **changes the on-air broadcast framing** (it adds the length prefix). A
patched receiver falls back to the legacy behavior for an **unpatched sender**, so
a broadcast only decodes cleanly when **both ends are patched**. Mixing one
patched station with stock peers reproduces the original symptoms in the receive
direction — patching only the *receiver* does nothing, because the length has to
be put on the air by the *sender*.

## Why it looked like a Linux-only problem
Mercury is the same code on both platforms, so the defects exist identically on
Windows and Linux. The Linux/Wine station merely **exposed** them:
- it was operated **receive-before-transmit** (triggering defect #1), and
- **Wine renders the padding as squares** where native Windows hides them
  (defect #2).

Windows stations in active two-way nets transmit broadcasts in normal operation
(latching the format) and hide the trailing nulls, so the symptoms are masked
there.

## Validation
With **all** stations on the patched build, received broadcasts arrived
**immediately** (no transmit-first) and **trimmed** — e.g.
`Sending KISS frame to client: kiss_cmd=0x01 payload=26` / `payload=22`, the real
message lengths with no padding.
