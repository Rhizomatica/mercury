# Using Mercury with Reticulum (RNS)

[Reticulum](https://github.com/markqvist/reticulum) is a cryptography-based
networking stack for building resilient, self-routing mesh networks over
almost any medium — including sub-kilobit HF radio channels.  Projects like
[reticulum-over-hf](https://github.com/RFnexus/reticulum-over-hf) already
bridge Reticulum to HF software modems (FreeDV, fldigi, modem73,
freedvtnc2); Mercury slots into the same ecosystem and brings low-SNR OFDM
data modes (usable down to about −7 dB SNR in 200 Hz), link adaptation, and
HARQ.

This document describes the possible integration architectures, gives
working configurations, and states the real constraints (MTU, modes, PTT).
Architecture 1 has been verified end-to-end on the two-instance ALSA
loopback testbed (`utils/loopsim`) with Reticulum 1.3.5: live RNS announces
were carried over the simulated RF channel in DATAC1 frames and processed by
the receiving RNS instance.

## Mercury's two data planes

Mercury exposes two independent data paths (see [TNC.md](TNC.md)):

| Path | Port (default) | Semantics | Framing |
|---|---|---|---|
| **Broadcast** | 8100 (`-b`) | Connectionless one-way datagrams, no ACK | **KISS over TCP** |
| **ARQ** | 8300/8301 (`-p`) | Connection-oriented reliable byte pipe (VARA-compatible TNC) | control: CR-terminated ASCII; data: raw bytes |

Reticulum itself is connectionless and does its own routing, retransmission
and encryption end-to-end, so the **broadcast plane is the natural fit** —
Mercury acts as a "sound-modem TNC" exactly where Direwolf or freedvtnc2
would sit.  The ARQ plane fits a different niche: a nailed-up point-to-point
backbone link between two fixed stations.

---

## Architecture 1 — RNS over the broadcast port (KISS over TCP)

**Recommended. Verified working.**  True multipoint: any station that
decodes a frame gets it, Reticulum handles addressing, routing, dedup and
announces above.

```
┌──────────┐  KISS/TCP   ┌───────────┐        ┌─────────┐
│   rnsd   │────────────▶│  Mercury  │──PTT──▶│  radio  │─── HF
│ (TCPClientInterface,   │ broadcast │  audio │         │
│  kiss_framing=True)    │ port 8100 │        └─────────┘
└──────────┘             └───────────┘
```

### MTU: pick the right Mercury mode

Reticulum requires an interface MTU of **500 bytes**.  Mercury broadcast
frames are fixed-size per mode, and 3 bytes are consumed by the Mercury
header + length prefix.  **Oversized KISS frames are truncated/dropped — the
broadcast plane does not fragment.**  Usable datagram sizes:

| `-m` index | Mode | Frame bytes | Usable (−3) | Payload rate | RNS-capable? |
|---|---|---|---|---|---|
| 0 | DATAC1 | 510 | **507** | 980 bps | **yes** |
| 9 | DATAC17 | 1180 | **1177** | 1410 bps | **yes** |
| 10 | QAM16C2 | 1213 | **1210** | 3100 bps | **yes** (high SNR only) |
| 1 | DATAC3 | 126 | 123 | 321 bps | no (too small) |
| 3 | DATAC4 | 54 | 51 | 87 bps | no |
| 7 | DATAC15 | 30 | 27 | 68 bps | no |

So run Mercury with **`-m 0` (DATAC1)** for the widest SNR envelope, or
`-m 9`/`-m 10` on strong links.  The default (`-m 1`, DATAC3) is **too
small** for Reticulum packets.  (See [MODES.md](MODES.md) for the SNR
performance of each mode.)

### Mercury side

```sh
# DATAC1 broadcast, Mercury keys the radio via Hamlib:
mercury -m 0 -i plughw:0,0 -o plughw:0,0 -R <hamlib_model> -A /dev/ttyUSB0 -b 8100
```

Mercury must do the PTT itself here (Hamlib `-R`/`-A`, or `-S` on
sbitx-based stations): there is no client in this architecture that could
react to the `PTT ON`/`PTT OFF` control-port lines.

### KISS command-byte compatibility (older Mercury builds)

Reticulum frames broadcasts with the standard KISS data command byte
(`0x00`), while VarAC uses VARA's compressed-callsign framing (`0x01`).
Current Mercury carries the sender's framing over the air in a broadcast
header extension bit (`BCAST_EXT_KISS_STD`) and delivers each received
frame to the local client with the **sender's own command byte** — so
stock Reticulum works against the broadcast port directly, and VarAC
behavior is unchanged.  Both stations must run a Mercury build with this
feature (newer than 1.9.10).

**Older builds** always deliver received frames with command byte `0x01`,
which Reticulum's KISS decoder silently discards — RNS→Mercury works but
Mercury→RNS does not.  For those, run this small shim between rnsd and
Mercury (it rewrites the command byte of Mercury→client frames to `0x00`
and passes the client→Mercury direction through untouched):

```python
#!/usr/bin/env python3
# kiss_shim.py <listen_port> <mercury_broadcast_port>
import socket, threading, sys

FEND = 0xC0

def pump(src, dst, rewrite_cmd):
    expect_cmd = False
    try:
        while True:
            data = src.recv(4096)
            if not data: break
            if rewrite_cmd:
                out = bytearray(data)
                for i, b in enumerate(out):
                    if b == FEND: expect_cmd = True
                    elif expect_cmd:
                        out[i] = 0x00; expect_cmd = False
                data = bytes(out)
            dst.sendall(data)
    except OSError: pass
    finally:
        try: dst.shutdown(socket.SHUT_WR)
        except OSError: pass

listen_port, mercury_port = int(sys.argv[1]), int(sys.argv[2])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", listen_port)); srv.listen(1)
while True:
    cli, _ = srv.accept()
    merc = socket.create_connection(("127.0.0.1", mercury_port))
    threading.Thread(target=pump, args=(cli, merc, False), daemon=True).start()
    threading.Thread(target=pump, args=(merc, cli, True), daemon=True).start()
```

```sh
python3 kiss_shim.py 8110 8100 &
```

### Reticulum side

In `~/.reticulum/config`:

```ini
[interfaces]
  [[Mercury HF]]
    type = TCPClientInterface
    interface_enabled = True
    kiss_framing = True
    target_host = 127.0.0.1
    target_port = 8100        # Mercury broadcast port (or the shim port on older builds)
    bitrate = 980             # match the Mercury mode (DATAC1)
```

Set `bitrate` to the mode's payload rate so Reticulum's transfer-time
estimates and announce rate limiting behave sensibly.

### Properties and tuning

- **Multipoint mesh**: any number of stations on the same channel;
  Reticulum's transport does routing and dedup.  This is the architecture
  that scales beyond two stations.
- **No link-layer ARQ**: a lost frame is recovered end-to-end by Reticulum
  (link KEEPALIVE, resource retries), which is slow on HF.  Prefer robust
  modes over fast ones on marginal paths.
- A DATAC1 frame is ~4.2 s + ~0.7 s preamble of airtime; a single RNS
  announce fits in one frame (measured: 167-byte announce → one 510-byte
  frame).  Keep announce traffic minimal (`announce_cap` in the interface
  config; leave the default unless you know better).
- Half-duplex collisions are possible (no CSMA on HF); keep the number of
  chatty destinations low.

---

## Architecture 2 — point-to-point ARQ backbone (bridge / custom interface)

Use Mercury's ARQ plane — the VARA-compatible TNC ([TNC.md](TNC.md)) — as a
**reliable byte pipe between two fixed stations**, and present it to
Reticulum as an interface.  This is the same pattern the Reticulum ecosystem
uses for VARA modems, and it is where Mercury's strengths live: OLLA link
adaptation, HARQ soft combining, and ACK robustness on weak/asymmetric
paths.

```
station A                                            station B
rnsd ── bridge ── :8300/:8301 Mercury ──HF──► Mercury :8300/:8301 ── bridge ── rnsd
        (MYCALL/LISTEN/CONNECT + data pipe)
```

The bridge is a small program that:

1. connects to Mercury's control (8300) and data (8301) ports;
2. issues `MYCALL <call>`, `LISTEN ON` at startup;
3. on outbound traffic with no session: issues `CONNECT <mycall> <peer>`,
   waits for `CONNECTED`;
4. while connected, shuttles bytes between Reticulum and the data port
   (adding its own length framing, since the ARQ pipe is a byte stream and
   Reticulum needs packet boundaries — HDLC-style framing as used by
   Reticulum's `PipeInterface` is the natural choice);
5. tears down (`DISCONNECT`) after an idle timeout, returning to `LISTEN`.

On the Reticulum side this can be a `PipeInterface` command (simplest) or a
custom Python interface module (cleanest).  No reference implementation
exists yet — treat this section as a design pattern; contributions welcome.

Trade-offs vs Architecture 1:

- **Reliable link layer**: Mercury retransmits, adapts the payload mode
  (DATAC15 → … → QAM16C2) and soft-combines retransmissions — far better
  goodput than end-to-end recovery on marginal links.
- **Two stations only** per session, ~10 s connect latency, and the
  connection-oriented semantics don't match Reticulum's datagram model —
  best for a nailed-up backbone hop (e.g. village ↔ gateway) rather than a
  general mesh channel.
- PTT: Mercury should key the radio itself (`-R`/`-A`/`-S`). If the radio is
  driven by another program, run Mercury with radio type `-1` and let that
  program act on the `PTT ON`/`PTT OFF` control-port lines.

---

## Architecture 3 — hybrid and other variants (future)

- **Hybrid**: use the broadcast plane for RNS announces/discovery and bring
  up an ARQ session on demand for bulk `Resource` transfers between two
  stations.  Requires a smarter bridge that owns both planes; nothing exists
  today.
- **Via BPQ32**: Mercury already works as a VARA modem under BPQ32, and
  Reticulum can ride AX.25/KISS out of BPQ.  Possible, but double framing
  and an extra hop of complexity for no benefit — not recommended.

## Choosing

| | Architecture 1 (broadcast/KISS) | Architecture 2 (ARQ bridge) |
|---|---|---|
| Topology | multipoint / mesh | fixed point-to-point |
| Loss recovery | Reticulum end-to-end | Mercury ARQ + HARQ (fast, local) |
| Modes | DATAC1/DATAC17/QAM16C2 only (MTU) | full adaptive ladder incl. DATAC15 fringe modes |
| Status | **verified working** | design pattern, needs a bridge implementation |
| Best for | community mesh, several stations | maximum robustness on one marginal backbone hop |

Start with Architecture 1.  If you have exactly two stations and a marginal
path, Architecture 2 will move data where Architecture 1 stalls.

## Operational notes

- **Station identification**: broadcast frames carry no callsign (`MYCALL`
  only affects ARQ).  Identify per your license conditions by whatever means
  your administration requires.
- **Encryption**: all Reticulum traffic is end-to-end encrypted by design
  and cannot be sent in the clear.  On amateur bands this may conflict with
  national regulations; for licensed fixed/community HF services (Mercury's
  primary deployment context) it is normally fine.  Know your rules.
- **Duty cycle**: Mercury OFDM modes transmit at full duty cycle; make sure
  the transceiver is rated for it (reduce power if not).

## Appendix — bench test procedure (no radio needed)

The end-to-end verification of Architecture 1 was done like this, on one
Linux host with `sudo modprobe snd-aloop` (see `utils/loopsim/README.md`):

1. Start two Mercury instances wired through the ALSA loopback, both at
   DATAC1, broadcast ports 8100/8200 — same wiring as
   `utils/loopsim/run_loopsim.sh` with `-m 0` added and a clean channel.
2. Create two Reticulum config dirs with the interface config above
   (`target_port` 8100 / 8200, `share_instance = No` in `[reticulum]`).
3. On instance B, announce a destination every 40 s; on instance A, register
   an announce handler (see the Reticulum announce example; note that
   `aspect_filter` must be the **full** destination name, e.g.
   `"myapp.aspect"`).
4. Result with RNS 1.3.5 against the broadcast ports directly (no shim):
   every announce was delivered — Mercury logs
   `[tcp-bcast] Sending KISS frame to client: kiss_cmd=0x00 payload=167`,
   and the RNS handler fires on instance A.

## See also

- [TNC.md](TNC.md) — the VARA-compatible control/data protocol and the
  broadcast port framing details.
- [MODES.md](MODES.md) — per-mode SNR performance, frame sizes, data rates.
- [ARQ.md](ARQ.md) — the ARQ protocol Architecture 2 rides on.
- [Reticulum manual — interfaces](https://markqvist.github.io/Reticulum/manual/interfaces.html)
- [reticulum-over-hf](https://github.com/RFnexus/reticulum-over-hf) — HF
  practices for Reticulum (NVIS, antennas, expectations).
