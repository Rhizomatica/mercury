# Mercury TNC Command Reference

Mercury exposes a **VARA-compatible TCP TNC interface** for client applications.
Two TCP ports are used:

| Port             | Default | Purpose                                    |
|------------------|---------|--------------------------------------------|
| Control port     | 8300    | Text commands and asynchronous status lines |
| Data port        | 8301    | Raw application data (binary payload)       |
| Broadcast port   | 8100    | KISS-framed broadcast frames (separate)     |

The control and data ports are `base_port` and `base_port + 1` respectively;
override with `-p <base_port>`.  The broadcast port is independent; override
with `-b <port>`.

**One client at a time.** While a client is connected to the control port,
another connection to it is closed at once, and the connected client keeps
the TNC.  The data port works the same way.  Mercury logs the first refusal
as a warning naming both addresses, then a count once a minute while the
refusals continue:

    control connection from 127.0.0.1:43866 refused: 127.0.0.1:43850 is connected and keeps the TNC. ...

That line means two host programs, or two ports of one host, are set up for
the same Mercury.  Before this rule, the newcomer took over and closed the
connected client.  So two BPQ32 ports pointed at one Mercury took it from
each other every few seconds, and each takeover would have ended a session in
progress (KG9NI, 2026-11-12: 16:26 to 18:11).

A client that has gone is replaced as before.  When a host program restarts
on the same machine, the OS has closed its old connection, so the new one
gets in at once.  A connection whose other end vanished without closing (a
remote host whose network went down) is held until TCP keepalive gives up on
it: 30 s idle, then 3 probes 10 s apart on Linux (about a minute), or 10
probes 1 s apart on Windows (about 40 s).  After that the next connection
gets in.

**A web browser on the control port.** The control port is not a web page.
A browser pointed at it (`http://<host>:8300`) gets a short HTTP 400 reply
saying so, and only its own connection is closed: a host's data connection
stays, and the host gets the control port at once.  Mercury logs

    control connection from 192.168.1.20:52344 is a web browser: closed. ...

Mercury's web interface, when it is enabled (`-G`), is on the UI port, 10000
by default (`-U`).  Before this, the browser kept the control port while it
stayed connected (K7EK, 2026-11-14), each line of its request answered
`WRONG`, and its leaving closed the host's data connection.

A host program that hangs without closing its connection keeps the port for
good.  This happens on the same machine: the program is stuck, but its OS still
answers the keepalive probes for it, so keepalive never gives up.  A new
instance is refused, with the warning above, until the old one is gone.  So
when you restart a hung host program, BPQ32 for example, kill the old
process first, then start the new one.  (Before this rule, the new instance
took over.)

All control commands and responses are **CR-terminated ASCII** (`\r`).

Command names are matched as **whole words**: `ABORTX` is not `ABORT`, and
`MONITORING` is not `MONITOR`.  Extra words where a command takes none, or a
word after its last argument, are `WRONG` (`ABORT SESSION`, `DISCONNECT
NOW`, `MONITOR ON junk`); the no-ops `COMPRESSION`, `IGNOREKISSDCD` and
`GATEWAY` take whatever follows without checking it.

---

## Commands (Client → Mercury)

Commands are sent on the **control port**.

### MYCALL

Set the local station callsign and optional secondary callsigns.

```
MYCALL <callsign> [<secondary1> <secondary2> ...]\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.  After `OK`, an
asynchronous `REGISTERED <callsign>\r` notification confirms the callsign is
recognised (VARA compatibility).

The first token is the **primary callsign**, used as the
source address for outgoing `CALL` and `ACCEPT` frames.  Any additional
space-separated tokens are registered as **secondary callsigns** — Mercury will
accept incoming `CALL` frames addressed to any of them, up to a maximum of
`CALLSIGN_MAX_SECONDARY` (currently 4) secondary callsigns.

Sending a new `MYCALL` command replaces the primary callsign and **clears all
previously registered secondary callsigns**.

Must be set before `LISTEN ON` or `CONNECT`.

The primary callsign is what this station's `CALL` frames carry, coded over
the letters `A`–`Z` (either case), the digits and `-`, in at most 14
characters.  A primary of more than 15 characters is `WRONG`: it used to be
cut short, which named a different station.  Up to 15, a primary outside
the CALL's alphabet is still taken (`OK`): the station can
answer calls, since an `ACCEPT` carries only CRC16s of the callsigns, but it
cannot make them: Mercury logs a warning saying so, and every `CONNECT`
after it answers `WRONG` until a `MYCALL` gives a primary a `CALL` can
carry.  (A callsign set only in the ini is not checked.)  Secondary callsigns
go on the air only as a CRC16 and have no such limit.  Long station names and
any SSID work.

Examples:

```
MYCALL VK2XYZ\r
MYCALL VK2XYZ VK2XYZ-1 VK2XYZ-2\r
```

---

### LISTEN

Enable or disable listening for incoming ARQ connections.

```
LISTEN ON\r
LISTEN OFF\r
LISTEN CQ\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

When enabled, Mercury enters the LISTENING state and will accept incoming
CALL frames addressed to the local callsign (or any callsign if PUBLIC is ON).

`LISTEN CQ` is treated as `LISTEN ON`.  Any other
argument is `WRONG`: the argument is compared as a whole word, in upper case,
as it is for `PUBLIC` and `CHAT` (`CWID` takes either case).

---

### PUBLIC

Accept calls addressed to any callsign (promiscuous mode).

```
PUBLIC ON\r
PUBLIC OFF\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

Default is OFF.  When ON, Mercury accepts incoming CALL frames regardless
of the destination callsign.

---

### BW (Bandwidth)

Set the maximum ARQ bandwidth.

```
BW2300\r
BW500\r
BW2750\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

- **BW2300** — Full bandwidth: MFSK, MFSK2K, DATAC4, DATAC3, DATAC1,
  DATAC17 and QAM16C2.  If the ACCEPT goes on the 500 Hz MFSK (the deep
  floor), the session keeps MFSK500 as rung 0.
- **BW500** — Narrow bandwidth: the 500 Hz MFSK floor (MFSK500), DATAC15,
  DATAC4, DATAC3 and the 9-carrier DATAN1, QAM16N1 and QAM16N2.  The
  wideband rungs are not used.
- **BW2750** — Accepted for VARA compatibility, negotiated and reported as
  `2750`: the BW2300 ladder, deep floor included, with the 44-carrier
  DATAC17W and QAM16C2W as rungs 5 and 6.

The per-rung table is in [MODES.md](MODES.md).

Both directions use the negotiated ladder for the whole session
(`car_set_bandwidth` in `datalink_arq/carousel.c`).
A `BW` command during a session applies to the next one.

During connection setup, Mercury advertises the local BW token in `CALL` and
returns the negotiated token in `ACCEPT`. If either side is `BW500`, the session
stays at `500` on both ends. If both sides are wide, the session keeps the lower
wide token (`2300` or `2750`) and `CONNECTED ... BW` reports that negotiated value.

---

### COMPRESSION

No-op for VARA client compatibility.

```
COMPRESSION ON\r
COMPRESSION OFF\r
```

**Response:** `OK\r` (always).

Mercury does not use compression; this command exists so VARA-compatible
clients can connect without errors.

---

### CHAT

Enable chat-optimized mode (VARA compatibility).

```
CHAT ON\r
CHAT OFF\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

`CHAT ON` implicitly enables `LISTEN ON`, placing Mercury in the LISTENING
state.  This matches VARA behavior, where chat applications expect the modem
to be ready for incoming connections after `CHAT ON`.

`CHAT OFF` is acknowledged but has no effect — Mercury does not currently
differentiate chat and file-transfer timing.  Any other argument is `WRONG`.

---

### P2P

No-op for VARA client compatibility.  Pat sends `P2P SESSION` before a
peer-to-peer `CONNECT`, as it sends `WINLINK SESSION` before a gateway one.

```
P2P\r
P2P SESSION\r
```

**Response:** `OK\r`; `WRONG\r` with any other argument.

---

### WINLINK SESSION

No-op for VARA client compatibility.  Pat sends it (or `P2P SESSION`)
before each `CONNECT`; Mercury runs both kinds of session the same way.

```
WINLINK SESSION\r
```

**Response:** `OK\r` (always).

VARA gives it a meaning: connect retries on a 4.0 s cycle, for RMS gateways
that dwell 4 s on a frequency (`P2P SESSION`: 4.6 s).  Mercury does not act
on it yet.

---

### CLEANTXBUFFER

Drop the host's bytes not yet sent, and keep the session.

```
CLEANTXBUFFER\r
```

**Response:** VARA's lines, not `OK`, with a space as a VARA-compatible
modem tested against VARA HF 4.9.0 sends them (VARA's command reference
writes them without one):

- `CLEANTXBUFFER OK\r` when unsent bytes were dropped
- `CLEANTXBUFFER BUFFEREMPTY\r` when there were none
- `CLEANTXBUFFER FAILED\r` when the command could not be queued

The reply comes once the clear is done.  Kept are the frames already on the
air and waiting for an ACK, and what the carousel already took into its open
block (up to about 2 kB); `ABORT` drops those too.  Bytes the host wrote
that Mercury has not yet read from the data socket are out of reach and
are still sent.  BPQ32 sends `CLEANTXBUFFER` right before `DISCONNECT` when
a session reaches its time limit, so the disconnect does not wait for the
backlog to be sent.

---

### CWID

Accepted so a host's start-up does not fail.  Mercury sends no CW ID and no
periodic ID.  A `CALL` or `CQ` frame carries the sending station's callsign;
other frames do not (an `ACCEPT` carries only CRC16s of the callsigns, data
and `DISCONNECT` frames only the session id).  Identifying the station as
your licence requires is up to the operator or the host program.

```
CWID ON\r
CWID OFF\r
```

**Response:** `OK\r` for `ON` or `OFF`, in either case, `WRONG\r` otherwise.
`CWID ON` logs an info line that no CW ID is sent.  Pat sends `CWID ON` at
every start.

---

### GATEWAY

Accepted and ignored: Mercury runs a gateway and a client the same way.

```
GATEWAY [<argument>]\r
```

**Response:** `OK\r`, whatever follows.

---

### DRIVELEVEL

Set the drive level without keying the transmitter.

```
DRIVELEVEL <-dB>\r
```

**Response:** `OK\r` for a number from `-30` to `0` (a fraction is rounded
to a whole dB: `-9.5` gives `-10`), `WRONG\r` for anything else, which
leaves the level as it was.

The value is attenuation, never gain, for both transmissions:

- The [tuning carrier](#tune) keeps the level `TUNE` set (`TUNE ?` still
  reports that level, −15 dBFS by default) and goes out that many dB below
  it: `DRIVELEVEL -6` puts it 6 dB lower, and `DRIVELEVEL 0` leaves it as
  it is.  A carrier already on changes to it.
- Data frames take it as attenuation on top of the TX gain set in the GUI
  or `audio:tx_gain_db`: `DRIVELEVEL -6` sends data 6 dB below that gain,
  and `DRIVELEVEL 0` sends it at that gain.

So a host can lower the drive, to protect an amplifier for example, but can
never raise it past what the operator set.  It is not saved: each start
begins at 0 dB.

---

### CONNECT

Initiate an ARQ connection to a remote station.

```
CONNECT <mycall> <theircall>\r
```

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error,
including anything after `<theircall>` and a callsign longer than 15
characters (it used to be cut short, which named a different station).
The `CALL` carries [`MYCALL`](#mycall)'s primary callsign; `<mycall>` is
reported back in `CONNECTED`.  When the last `MYCALL` gave a primary a
`CALL` cannot carry, `CONNECT` is `WRONG` (before, `OK` and then nothing
was sent).

The digipeated form, `CONNECT <mycall> <theircall> VIA <digi1> [<digi2>]`,
is `WRONG`: Mercury has no relay.  Before, it was answered `OK` and
connected direct, the path dropped.

Mercury will transmit CALL frames on DATAC16 (the control mode) -- the first
two, then MFSK and DATAC16 alternately, so a station that cannot hear DATAC16
is still reached -- advertising the local BW token,
and wait for an ACCEPT carrying the negotiated session BW.
On success, the asynchronous response
`CONNECTED <sourcecall> <destcall> <bandwidth>\r` is sent on the control port,
preserving the same call order as the original `CONNECT` command on both peers.

Example:
```
CONNECT AAAA BBBB\r
```

---

### CQFRAME

Transmit a compact CQ frame on the control mode (DATAC16).

```
CQFRAME <sourcecall> <bandwidth>\r
```

`<bandwidth>` must be one of `500`, `2300`, or `2750`; it is not optional,
and nothing may follow it.  `<sourcecall>` is coded into the CQ frame over the
same alphabet as [`MYCALL`](#mycall)'s primary (`A`–`Z`, digits and `-`);
a CQ has room for 15 characters, one more than a `CALL`.

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error.

For VARA compatibility, Mercury also emits `PENDING\r` when the CQ frame
starts transmitting and `CANCELPENDING\r` once that CQ transmission is done.

---

### DISCONNECT

Terminate the current ARQ session.

```
DISCONNECT\r
```

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error.

`DISCONNECTED\r` is sent on the control port when the session has
closed, as VARA does ("Disconnect the link, once the TX buffer is empty").
Mercury first delivers what is still queued or unconfirmed, for up to three
exchanges at the session's rung (minutes at the MFSK floor), then sends
DISCONNECT frames to the peer and waits for its answer.  With no session up,
`DISCONNECTED\r` is sent at once.  A host that cannot wait sends `ABORT`.
Either way the host hears one `DISCONNECTED\r` per session.  A session
the peer has just ended still owes its notice until Mercury's reply to the
peer's DISCONNECT is on the air, so a `DISCONNECT` then waits for that
notice too.

Up to 1.9.22 `DISCONNECTED\r` was sent at once, before the air side ended.
A scanning BPQ32 restarts its scan 15 s after it, and its next `LISTEN OFF`
ended the session before the peer had heard the DISCONNECT, leaving the
peer to time out (240 s).  BPQ32 keeps the radio while it waits for
`DISCONNECTED`, and sends `ABORT` after 30 s.
It does not wait to deliver when nothing has been heard from the peer
for 120 s, and a delivery under way stops once that silence is reached.

While that delivery is under way:

- a second `DISCONNECT` ends the session at once, as `ABORT` does;
- a `CONNECT` ends it too: the old session's unsent bytes are dropped,
  its DISCONNECT goes out, and the new call is placed after it.  A
  `DISCONNECT` before that call is placed cancels the call.

**Winlink Express P2P (opt-in).** Winlink Express sends `DISCONNECT` in a
P2P session 5 minutes after it last wrote to the data port ("P2P session
inactivity timeout"), even while the modem is still sending that data.
`BUFFER`, `PTT` and `SN` lines do not reset its timer, so a message that
takes longer than 5 minutes to send is lost: the session ends and the
message stays in the Outbox.  VARA HF behaves the same way (#392).  The
timer restarts with each write, so every long message brings its own
`DISCONNECT`, and one that takes over 10 minutes brings one every 300 s.

With `[tnc] absorb_idle_disconnect_s = 240`, a `DISCONNECT` in a session is
answered `OK` and otherwise ignored when nothing has been written to the
data port for that long (or since the session came up), as often as it
comes.  Winlink then carries on.  A real end of session comes seconds
after the host's last write, never with host data held back, and goes
through.  A `DISCONNECT` that comes while Mercury is holding the host's
data back because the TX queue is full is absorbed too.  Winlink's timer
starts when its write returns, but Mercury reads that data only as the
queue has room, so its idle time can start minutes later.  A `DISCONNECT`
within 60 s of the previous one always goes through, so a host or operator
that means it is heard on the second.  Once one has gone through, none is
absorbed for the rest of that session, so a `DISCONNECT` during its drain
still ends the session at once.  `ABORT` ends the session at once as usual.
The default is 0 (off), which keeps VARA's behaviour.  Accepted values are
0 or 30–3600 s.

---

### ABORT

Dirty disconnect — drop everything still held and end the session at once.

```
ABORT\r
```

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error.

`DISCONNECT` first delivers what is still queued: unsent bytes, and the
pieces the peer has not yet confirmed.  `ABORT` does not wait.  It discards
the unsent TX data, the unconfirmed pieces and received data not yet read,
then sends one DISCONNECT frame straight away (after any frame already on
the air, and clear of the peer's reply window), so the peer is told rather
than left to time out.  To a peer that last reported hearing the control
mode below -3 dB, that DISCONNECT goes twice, back to back in one keydown
(3.7 s more on the air): on 15 Nov 2026 a single one was lost twice at -3 to
-9 dB, and the peer waited out its 240 s watchdog.  It is not retried: the caller is free once the
peer answers or one retry interval passes, and the callee once its
DISCONNECT is out.  The callee's waits for its own next slot only so
long: if none comes in time to be on the air and done within 15 s of the
`ABORT`, it keys out of turn, because a scanning BPQ32 takes the radio back
as it sends `ABORT` (see [Scanning hosts](#scanning-hosts-bpq32)).  The one
DISCONNECT is never keyed over a station Mercury hears on the air, as it is
not retried and the peer could not hear it: the caller's keeps to the
callee's slots, and the callee's out-of-turn one waits for a clear channel
(the slot after the caller's keydown carries it).  A keydown of Mercury's
already on the air at the `ABORT` still runs to its end first.
`DISCONNECTED\r` is sent on the control port at once, and it is the
session's only one: the end of the teardown that follows sends no second
`DISCONNECTED\r`.

After that `DISCONNECTED\r`, while the one DISCONNECT is still to go out,
Mercury sends `PENDING\r` (again every 20 s, as when answering a call: see
`PENDING`), then `CANCELPENDING\r` once the DISCONNECT is out or the
teardown has ended. As the callee, the teardown ends about 30 s after the
`ABORT` at the latest: a DISCONNECT that never finds a clear channel is then
dropped, and the `CANCELPENDING\r` comes without it. A 60 s limit on the hold is only a
backstop.  The reason
is a scanning BPQ32: it restarts its scan 15 s after it sends `ABORT`, and
the hop's `LISTEN OFF` takes the radio.  At BW500 the peer's frame on the
air can be 26 s.  On 2026-11-13 the hop came 16.6 s after K7EK's `ABORT`,
while KG9NI was still sending; the DISCONNECT was never sent, and KG9NI
waited out its 240 s watchdog.  BPQ32 refuses a hop while a connect is
pending (`PENDING` sets it; `DISCONNECTED` or `CANCELPENDING` clears it), so
the `PENDING` after the `DISCONNECTED` keeps the radio on the channel until
the DISCONNECT is out.  Mercury holds a scanner the same way while a CQ is on
the air.

An `ABORT` during a teardown already under way (after a `DISCONNECT`) cuts
it short the same way: no more retries.  With its DISCONNECT already out,
the caller is free when the retry interval running ends (or the peer
answers), and the callee at once.  Its `DISCONNECTED\r` too comes at once
and is the session's only one.

---

### VERSION

Query the modem identification string.

```
VERSION\r
```

**Response:** two lines on the control port:

```
VARA version 4.9.0 registered\r
VERSION VARA HF v4.9.0\r
```

This command exists for VARA-compatible clients that check the modem version
at startup, and two of them want different lines:

- VarAC needs `VARA version 4.9.0 registered` exactly. It comes first and
  is unchanged.
- Pat waits, with no timeout, for a line starting with `VERSION` (or
  `WRONG`), and ignores any other. Before the second line was added, Pat's
  VARA HF start-up got past the wait only while `CWID ON` was answered
  `WRONG`.
- The second line has real VARA's form, `VERSION VARA HF v<n>`. VarAC
  parses it as well, and a shorter `VERSION 4.9.0` made it log "Failed to
  read VARA version".

---

### IGNOREKISSDCD

No-op for VARA client compatibility.

```
IGNOREKISSDCD\r
```

**Response:** `OK\r` (always).

---

### BUFFER

Query the number of bytes pending in the ARQ transmit buffer.

```
BUFFER\r
```

**Response:** `BUFFER <bytes>\r` on the control port, to every query, also
when the count is the one the last `BUFFER` line said.  The unsolicited
reports ([BUFFER](#buffer-1) below) go only when the count changes, or every
50 s in a session while bytes wait; an answered query counts as the last
line for them.

---

### SN

Query the last measured signal-to-noise ratio.

```
SN\r
```

**Response:** `SN <value>\r` (e.g., `SN 8.2\r`) on the control port.

---

### BITRATE

Query the current throughput estimate.

```
BITRATE\r
```

**Response:** `BITRATE (<code>) <bps> BPS\r` on the control port.  `<code>`
names the mode on the air: 1 DATAC1, 3 DATAC3, 4 DATAC4, 15 DATAC15,
16 DATAC16 (the control mode, while calling only), 17 DATAC17, 25 QAM16C2,
26 DATAC17W, 27 QAM16C2W, 28 DATAN1, 29 QAM16N1, 30 QAM16N2, 100 MFSK,
103 MFSK at 500 Hz (MFSK500), 104 MFSK at 2 kHz (MFSK2K), 110 PSK3.

Mercury also sends the line unprompted: in a session, for each frame on a
payload mode keyed or decoded; and while a `CONNECT` is calling, on the
first CALL and at least every 20 s until the call connects or ends, for the
carrier of the CALL out last.  The second keeps a host's idle timer from
running out while the CALLs go unanswered.  BPQ32 passes TNC lines it does
not handle itself to the session (`VARA} BITRATE ...`), and a forwarding
connect script may set the session's IDLETIME as low as 60 s: on air
(KG9NI, November 2026) calls were dropped 61-62 s after the last line,
before the first MFSK CALL had gone out.

---

### RETRIES

Override how many times Mercury repeats a CALL or an ACCEPT before giving up
on a connect.  Setting to 0 reverts to the defaults in
datalink_arq/arq_protocol.h.  (A connected session's own limits are the
carousel's; this does not change them.)

It also sets how many CALLs go on the fast control mode (DATAC16) before
the first MFSK CALL: `RETRIES/2`, at least 2 (the default) and at most 4.
DATAC16 CALLs are short, so a scanning station is more likely to catch one
while it is on the frequency, and any listener decodes them whatever its
bandwidth.  The cap keeps the first MFSK CALL, which a weak path needs,
from coming too late (#442).  Each DATAC16 CALL after the first is followed by
the MFSK floor's call interval, about 24 s (37.5 s at 500 Hz), because the
callee may answer it on the floor (#536), so the first MFSK CALL comes about
40 s after the first CALL with the default 2 fast CALLs, and about 95 s with
4 (about 135 s at 500 Hz).

However many CALLs `RETRIES` allows, no new one is keyed after
`arq:call_max_s` seconds of calling (default 300; 0 = no cap).  `RETRIES 20`
alone is about 8 minutes on the frequency.

After the fast CALLs, a caller at BW2300 or BW2750 calls on the 1 kHz MFSK
floor and then twice on the 500 Hz MFSK (27 s), which reaches about 3 dB
deeper.  A scanning gateway never catches a 27 s CALL, so a station that
mostly calls scanning gateways can set `arq:call_mfsk500 = false`: its CALLs
after the fast ones are then the 1 kHz MFSK and DATAC16 in turn.  See
[Calling a scanning gateway](#calling-a-scanning-gateway).

```
RETRIES 10\r
```

`RETRIES=10` and `CALLINT=10` are accepted too, since BPQ32 passes a port's init
lines to the TNC as written.  A command Mercury answers `WRONG` is logged as a
warning: `Command rejected (WRONG): <line>`.

---

### CALLINT

Override the interval in **whole seconds** between successive CALL/ACCEPT
frame retries during connection setup.  This has no effect on the
DISCONNECT's timing or on a session once it is established.

Send `0` to restore the compiled default (8 seconds).  The minimum
enforced value is 4 seconds; values below 4 are clamped to 4 to avoid
firing a retry before a valid ACCEPT can return over the air.

The default is per carrier: a CALL whose ACCEPT may come back on the MFSK
floor (a floor CALL, or a repeat on the control mode) waits the floor's
interval, about 24 s (37.5 s at 500 Hz), so as not to key over that 13.5 s
ACCEPT.  `CALLINT` overrides that too: below about 20 s a CALL may key
over a floor ACCEPT its decoders did not catch.

```
CALLINT 5\r
CALLINT 0\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

---

### MODE

Set the **listen mode** — the payload mode the station sits on while no ARQ
session is up.  This decides which broadcast frames the station can hear and
send, and takes the same index space as the `-m` startup option (use `-l` to
list them).

```
MODE 10\r          set the listen mode to QAM16C2
MODE\r             report the current listen mode
```

The query form answers `MODE <index> <name>\r`, e.g. `MODE 10 QAM16C2\r`, so
the reported index round-trips as the argument to a later `MODE` command.

**Response:** `OK\r` on success, `BUSY\r` if an ARQ session is up, `WRONG\r`
for an index that is out of range or not switchable at runtime.

Two limits are worth knowing:

* **Only accepted while the ARQ link is idle** (disconnected or listening).
  During a session the carousel owns the payload mode and would
  overwrite an operator setting within one RX-loop iteration, so the command
  answers `BUSY\r` rather than silently accepting a setting that is about to
  be discarded.
* **Only the pooled payload modes are runtime-switchable:** indexes 0
  (DATAC1), 1 (DATAC3), 3 (DATAC4), 7 (DATAC15), 9 (DATAC17), 10 (QAM16C2),
  11 (MFSK) and 12 (MFSK500).  DATAC16 is the ARQ control mode, and DATAC0 / DATAC13 /
  DATAC14 / FSK_LDPC are not payload modes at runtime — `-m` can still
  *start* on some of those, but they cannot be selected later, and `MODE`
  answers `WRONG\r`.

This does **not** change how ARQ connections behave.  Every session — inbound
or outbound — starts each direction on the rung the connect's SNR supports,
so a fast listen mode gives a later connection no head start, and a robust
one costs it nothing.  Setting the listen mode is about *broadcast*
reception and transmission while idle.

---

### BEACONMODE

Set the mode a broadcast client's **station messages** go on: frames of
packet type `0x07` (`PACKET_TYPE_STATION`, the HERMES position beacon), sent
on the broadcast port with KISS `0x03` (`CMD_MODEM_FRAME`).  The index space
is `-m`'s, as for `MODE`.  The listen mode stays as it is.

```
BEACONMODE 8\r     send station messages on DATAC16 (the default)
BEACONMODE -1\r    send them at the listen mode instead
BEACONMODE\r       report it
```

DATAC16 is the default because it is the most robust mode a compact (14-byte)
beacon fits that every running Mercury decodes: its control decoder is always
on it.  DATAC15 is as robust, but only stations whose listen mode it is decode
it.

The query form answers `BEACONMODE <index> <name>\r`, or `BEACONMODE -1
LISTEN\r`.

**Response:** `OK\r` on success, `WRONG\r` for an index other than 8
(DATAC16), one `MODE` accepts, or -1.

A station message is sent once, zero-padded to that mode's frame, after any
ARQ session or carousel has ended, and only when no decoder is on a frame
(after ~10 s of a busy channel, it is sent anyway).  One that does not fit the
mode's frame (a full-layout beacon on DATAC16, for example) goes at the listen
mode instead, as with `BEACONMODE -1`, and the log says so.  Like `MODE`, the
setting holds for the life of the modem, not of the client that sent it.  The best modes for it are the ones every listening
Mercury already decodes, whatever its listen mode:

- DATAC16, the control mode (14 bytes, 3.1 s);
- MFSK (98 bytes, 12.8 s) and MFSK500 (98 bytes, 25.6 s), the call listeners'
  modes, heard by a station with `LISTEN ON`.

On any other mode, only stations whose listen mode it is will hear it.
Mercury itself does not read the frame past its header: the layout is the
host's (hermes-radio-daemon `docs/BEACON-FORMAT.md`).

---

### MONITOR

Enable or disable **monitor mode** — the VARA-monitor equivalent: decode every
Mercury mode in parallel and report each frame that passes its CRC to the host,
without participating in the link.

```
MONITOR ON\r
MONITOR OFF\r
MONITOR\r
```

**Response:** `OK\r` on success, `WRONG\r` for an unrecognised argument.  The
bare query answers `MONITOR ON\r` or `MONITOR OFF\r`.

While monitor mode is on, the RX path runs one decoder per mode
(DATAC16, DATAC15, DATAC13, DATAC4, DATAC3, DATAC1, DATAC17, QAM16C2 and
MFSK) so a frame is heard whatever speed the transmitting station is using.
Each decoded frame produces an asynchronous `MONITOR ...` line on the control
port (see [Asynchronous Responses](#asynchronous-responses-mercury--client)).

Monitor mode is **read-only**.  The monitor decoders are separate codec
instances that never feed the ARQ state machine, so nothing is acknowledged,
no connect is auto-accepted, and the transmitter is never keyed on account of
what is heard.  The normal ARQ and broadcast planes keep running underneath,
so `LISTEN ON` / `CONNECT` still work exactly as before — the monitor is
purely additive.

**What it can and cannot see.**  A monitor station hears frames that carry a
plain CRC: `CALL`/`ACCEPT` connect exchanges, `CQ` frames, `DISCONNECT`, and
broadcast traffic.  A 1.9.x station's frames are shown as from "another
protocol version".  It does **not** yet decode the **carousel** data plane
(the only session type), because every in-session carousel frame carries
the session's CRC seed, which the monitor's decoders do not apply, so those
frames fail CRC and are not reported.  Watching a pair of `mercuryv2` stations, the monitor
therefore shows the connect, the disconnect and any CQ/broadcast, but nothing
of the data exchange in between.  Following carousel sessions (recovering the
seed from the `CALL`/`ACCEPT` and decoding the carousel framing) is planned as
a follow-up.

**CPU cost.**  Monitor mode trades CPU for coverage: each of the nine decoders
is a full OFDM/MFSK demodulator.  Measured idle on x86 through the FIFO
backend, `MONITOR ON` takes roughly four times the CPU of `MONITOR OFF`
(~12 % of a core → ~51–56 %), and returns to ~12 % on `MONITOR OFF`.  By the
usual Pi 4/x86 ratio that is on the order of 1.3–1.5 Raspberry Pi 4 cores, so
a small single-board station should enable it deliberately rather than leave it
running.  Two of the nine decoders (DATAC16 and MFSK) duplicate decoders the
modem already runs while idle; reusing their output would claw back part of
that cost and is a possible optimisation.

---

### BUSYDETECT

Turn the channel-busy detector on or off while Mercury runs, without a
restart.  It starts as `busy_detect` in `mercury.ini` sets it (off by
default); the command does not save the change.

```
BUSYDETECT ON\r
BUSYDETECT OFF\r
BUSYDETECT\r
```

**Response:** `OK\r` on success, `WRONG\r` for an unrecognised argument.  The
bare query answers `BUSYDETECT ON\r` or `BUSYDETECT OFF\r`.

Switched off while it reports the channel busy, the detector sends
`BUSY OFF\r` first, so the host is not left holding a BUSY.  Switched on again,
it measures a new noise floor.  The thresholds stay the ones the ini set.  See
[BUSY ON / BUSY OFF](#busy-on--busy-off).

---

### BUSYRESEED

Choose whether the busy detector measures a new noise floor at each scan hop.
It starts as `busy_reseed_on_hop` in `mercury.ini` sets it (off by default);
the command does not save the change.

```
BUSYRESEED ON\r
BUSYRESEED OFF\r
BUSYRESEED\r
```

**Response:** `OK\r` on success, `WRONG\r` for an unrecognised argument.  The
bare query answers `BUSYRESEED ON\r` or `BUSYRESEED OFF\r`.

With it on, each `LISTEN OFF` (the start of a scanner's hop) makes the detector
forget its floor, wait 1 s for the radio to retune, and measure the floor on
the new channel, keeping its BUSY/CLEAR verdict until the new channel decides
it.  With it off, one floor serves every channel.  Each way has a cost; see
[Scanning hosts](#scanning-hosts-bpq32).

---

### TUNE

Key the transmitter and hold a steady **1000 Hz tone**, so an antenna tuner
can find a match — and so an operator has a known reference to set drive
against.  Same syntax as VARA HF / VARA SAT.

```
TUNE -15\r      key TX, tone at -15 dBFS
TUNE ?\r        report the current drive level
TUNE OFF\r      stop the tone and unkey
```

The level is **absolute dBFS at the sound card**, in the range `-60` to `0`.
It deliberately does *not* pass through the modulator TX gain (`tx_gain`):
`TUNE -15` means −15 dBFS, not −15 dBFS scaled by whatever the modulator gain
happens to be — otherwise it would be useless as a reference for setting that
very gain.

Re-issuing `TUNE <level>` while a carrier is already running changes the level
and restarts the safety timer, which is what adjusting drive on a live tuner
requires.

> **The carrier always stops by itself after 60 seconds.**
> A tuning carrier is the one transmission with no protocol underneath it, so
> nothing else bounds it: a host that crashes, forgets `TUNE OFF`, or simply
> drops the control socket would otherwise leave the radio keyed — cooking the
> finals and jamming the channel.  Do not rely on the timer for normal
> operation; send `TUNE OFF` when the tuner is done.

`TUNE` is **refused** (`WRONG\r`) when:

- the level is outside `-60 .. 0` dBFS, or is not a number;
- an ARQ link is currently connected, or a burst is in flight — a tuning
  carrier and a data burst must never key the radio at the same time.

While the carrier is up, ARQ transmissions are suppressed; `TUNE OFF` (or the
timer) restores normal operation.  The usual `PTT ON\r` / `PTT OFF\r`
asynchronous lines bracket the carrier exactly as they do a data burst.

**Response:** `OK\r` on success, `WRONG\r` on error.
`TUNE ?` replies `TUNE <level>\r` (for example `TUNE -15\r`).

---


## Asynchronous Responses (Mercury → Client)

These are sent on the **control port** without a preceding command.

| Response                                    | Meaning                                      |
|---------------------------------------------|----------------------------------------------|
| `PENDING\r`                                 | Incoming connect request or outgoing CQ TX started |
| `CANCELPENDING\r`                           | Pending incoming connect cancelled or outgoing CQ TX completed |
| `CONNECTED <sourcecall> <destcall> <bandwidth>\r` | ARQ session established                |
| `CQFRAME <sourcecall> <bandwidth>\r`        | Compact CQ frame decoded                     |
| `MONITOR <mode> <kind> <detail> SNR=<snr>\r` | A frame decoded by monitor mode (passive)    |
| `DISCONNECTED\r`                            | ARQ session ended                            |
| `PTT ON\r`                                  | Radio transmitter keyed                      |
| `PTT OFF\r`                                 | Radio transmitter unkeyed                    |
| `BUFFER <bytes>\r`                          | TX buffer level update (periodic)            |
| `SN <value>\r`                              | SNR update                                   |
| `BUSY ON\r`                                 | Channel became occupied (busy detector)      |
| `BUSY OFF\r`                                | Channel became clear (busy detector)         |
| `BITRATE (<level>) <bps> BPS\r`            | Throughput update; while calling, every 20 s |
| `IAMALIVE\r`                                | Heartbeat (sent periodically while idle)     |
| `REGISTERED <callsign>\r`                  | Callsign registration confirmed (after MYCALL) |

### PENDING

Sent when Mercury detects an incoming ARQ connect request addressed to the
local station. This is an early warning so VARA-compatible host applications
can suspend scanning or other idle activity while the link setup is in progress.

Mercury sends it again each time it hears the same caller's CALL repeated while
it is still answering. BPQ32 holds its scan only six scan intervals per
`PENDING` (VARA.c: `ConnectPending = 6`), not until the connect completes, and
then hops, and the hop's `LISTEN OFF` cancels the incoming connect. On K7EK's
station (an 11 s dwell) that came 72-85 s after the first `PENDING`, in
5 incoming connects, one of them 17 s after Mercury had answered a repeated CALL.

Mercury also sends it again every 20 s for as long as it is answering, so a
scanning host keeps the channel exactly as long as Mercury does. Mercury
answers until its `ACCEPT` retries run out (`RETRIES`: that many retry
intervals after the last CALL heard, about 160 s for `RETRIES 20`), and ends
with `CANCELPENDING`. A repeated CALL is not enough on a weak path: on
2026-11-12 K7EK decoded 2 of a caller's 13 CALLs, and BPQ32 hopped 66 s after
the one `PENDING` while the caller went on calling for 2 minutes.

When the `ACCEPT` retries run out less than 90 s after the last `ACCEPT` went
out (the default 4 slots: about 43 s), the hold goes on until those 90 s are
up: a caller that decoded that `ACCEPT` is connected and polling, and its
next poll completes the connect after the retries have ended.  On 2026-11-23
(VE4KLM answering K7EK on 30 m) the `CANCELPENDING` came first and the
`CONNECTED` 31 s and 12 s later; a scanning host would have hopped in
between.  A host's `DISCONNECT`, `ABORT`, `LISTEN OFF` or `CONNECT`, or the
caller's DISCONNECT, ends the hold at once with `CANCELPENDING`.

Mercury sends it as soon as a decoder has locked onto a frame while LISTENING,
about a second into a CALL, not when the CALL has decoded: a scanning host
that stays 4 s on a frequency (RMS Trimode, with 0.5 s deaf while it changes)
almost never holds the whole 3.74 s of a control-mode CALL (13.5 s on the MFSK
floor), and `PENDING` is the only thing that keeps it there (#527).  It is
repeated every 20 s while the lock lasts, and ends with `CANCELPENDING` two
seconds after the lock is gone if no CALL for this station followed: a frame
for another station, or noise, holds the host for its length and that grace.
A lock that outlasts any frame (45 s) is noise and is released.  The 500 Hz
MFSK is left out of this, at every bandwidth (the floor a BW500 station
listens on, the second listener above it): on the 40 m band its carrier
sense held a false lock for a whole 27 s frame twice in 9 minutes.  A CALL
on it gets its `PENDING` when it decodes.

Mercury also sends `PENDING\r` when an outgoing `CQFRAME` actually begins
transmitting so VARA clients can treat CQ send as an in-progress operation.

And after an `ABORT`, following its `DISCONNECTED\r`, until the one
DISCONNECT frame is out or the teardown ends (see `ABORT`): a scanning host
does not take the radio before the peer has been told. This is a second hold,
apart from the answering hold above; both repeat `PENDING\r` every 20 s.

### CANCELPENDING

Sent when a previously pending incoming connect request does not complete and
Mercury returns to the idle/listening state: its `ACCEPT`s ran out (and 90 s
have passed since the last one, see `PENDING`), the host
sent `DISCONNECT` or `LISTEN OFF`, or the caller sent a `DISCONNECT` for that
call (it gave up calling before our `ACCEPT` reached it).  A scanning host
then resumes its scan at once rather than holding the channel.

Mercury also sends `CANCELPENDING\r` when an outgoing `CQFRAME` transmission
finishes, which matches the lifecycle expected by VARA clients such as varim.

varim before 1.13 also sends `ABORT` when `PTT ON` lasts about 20 s with no
`PTT OFF`, whatever else the TNC says meanwhile (#445).  varim 1.13 and later
allow 35 s, longer than any Mercury keydown (30 s at most), and need nothing
set.  With an older varim: a carousel round can be one keydown of 25-30 s, so
set `[arq] max_keydown_s = 16` on every station that runs it.  It holds each of that station's keydowns, a poll before a round
included, to 16 s; a peer without it can still key longer.  The BW500
floor's single MFSK frame is 27 s and is never split, so varim may still
abort a BW500 session that falls to its floor.

### CONNECTED

Sent when a session is successfully established (either outgoing CONNECT
or incoming CALL accepted). The `<bandwidth>` value is the negotiated session
BW token from the `CALL` / `ACCEPT` exchange (`500`, `2300`, or `2750`).
If either peer is `BW500`, both sides report `500`. If both peers stay wide,
Mercury reports the lower of the two wide tokens, preserving `2750` only when
both sides advertised it. `<sourcecall>` is always the station that initiated
the session, and `<destcall>` is always the station that was called.

### CQFRAME

Sent when Mercury decodes a compact CQ frame on the air.
`<sourcecall>` is the transmitting station and `<bandwidth>` is the BW token
advertised inside that CQ frame (`500`, `2300`, or `2750`).

### MONITOR

Sent once per frame that monitor mode decoded, on the control port:

```
MONITOR DATAC16 CALL FROM=K7EK BW=2300 SID=42 SNR=18.1\r
MONITOR DATAC16 ACCEPT BW=2300 SID=42 START=3 SNR=17.9\r
MONITOR DATAC16 DISCONNECT SID=42 SNR=18.4\r
```

`<mode>` is the mode the frame decoded on (`DATAC16`, `DATAC15`, `DATAC13`,
`DATAC4`, `DATAC3`, `DATAC1`, `DATAC17`, `QAM16C2` or `MFSK`).  `<kind>`
names the frame: `CALL`, `ACCEPT`, `CQ`, `DISCONNECT`, `CONTROL` / `DATA`
for another protocol version's session frames, `BCAST_CTRL` / `BCAST_DATA`
for broadcast frames, `STATION` for a station message (the beacon, detail
`LAYOUT=<n>`), or `FRAME` when the frame type is unknown.

`<detail>` carries what the monitor could recover:

- `CALL`: `FROM=<src> BW=<bw> SID=<n>` — the destination is not on the
  wire as a string, only its CRC, so a passive monitor cannot name it
- `ACCEPT`: `BW=<bw> SID=<n> START=<rung>` — neither callsign is on the wire
  in full
- a `CALL`/`ACCEPT` of another protocol version: `another protocol version
  (Mercury 1.9.x?)`
- `CQ`: `FROM=<src> BW=<bw>`
- `DISCONNECT`: `SID=<n>`
- `CONTROL` / `DATA`: `another protocol version (Mercury 1.9.x?)`
- broadcast: `LEN=<n>`

`SNR=` is the decoder's signal-to-noise estimate in dB at the moment the frame
was read (may be `0.0` when unknown).  These lines are display-only telemetry:
the queue is lossy, so under a flood a line may be dropped rather than stall a
modem thread.

### BUSY ON / BUSY OFF

Emitted by the optional channel-busy (occupancy) detector when it observes the
HF channel transition between clear and occupied, using VARA's exact wording so
existing VARA-compatible hosts (BPQ32, Winlink) act on them unchanged.
Edge-triggered: `BUSY ON\r` on clear→busy, `BUSY OFF\r` on busy→clear.

The detector is **disabled by default** and is enabled via the `[channel]`
section of `mercury.ini` (`busy_detect = true`), or while running with
[`BUSYDETECT ON`](#busydetect).  It is off because the hosts
that consume these notifications act on them: BPQ32 and Winlink hold
transmissions while BUSY is asserted, so a false or over-eager BUSY stops the
station transmitting.  BPQ32's scanner does not hold on it (see "Scanning
hosts" below), so turning it on for scanning costs outgoing connects and gains
nothing there.  Its sensitivity/timing knobs
(`busy_threshold_db`, `busy_hysteresis_db`, `busy_on_debounce_ms`,
`busy_hang_ms`) typically need on-air tuning per band/noise environment. When
disabled, these notifications are never sent. See `mercury.ini.example`.

The detector needs a receive filter at least ~1.8 kHz wide.  It measures
300-2700 Hz and takes its noise floor from the quieter part of that range, so a
narrower filter puts the floor under the filter skirt and an empty channel reads
BUSY (offline: 99% of the time behind a 600-2200 Hz filter, never at 500-2300 Hz
or wider).  A continuous signal covering most of the passband is eventually
absorbed into the floor and reads CLEAR after ~60 s (+12 dB) to ~95 s (+20 dB);
a step up in the noise itself cannot be told from such a signal and reads BUSY
for ~100-145 s until the floor catches up.

`BUSY ON` is the earliest notification a host can act on: it follows the carrier
by roughly the debounce time (~0.3 s by default), whereas `PENDING` cannot be
sent until a whole connect request has decoded — 3.74 s in the DATAC16 control
mode.  A host that scans could use the former to hold its dwell long enough to
receive the latter; BPQ32 does not.

### Scanning hosts (BPQ32)

What a scanning host needs from Mercury, as seen in KG9NI's logs (LinBPQ
scanning, Mercury 1.9.22, 10 incoming sessions, November 2026):

- **BPQ32 attaches Mercury as a VARA port** and holds its scan from `PENDING`
  until `DISCONNECTED` or `CANCELPENDING`, or until six scan intervals pass
  with no new `PENDING` (see `PENDING`: Mercury repeats it every 20 s while
  it answers).  In every session the scanner
  stopped once the CALL decoded, including a 32 s connect that needed a
  repeated CALL and a failed one it held for 43 s.
- **Each hop is `LISTEN OFF`, `LISTEN ON` and a `BW` command.**  The dwell
  was 10.1-10.2 s.  What the call decoders hold does not survive them:
  - a `LISTEN OFF` with no session up drops every partly received frame:
    the DATAC16 decoder's, the payload decoder's and the two MFSK call
    listeners', sync included.  A partly received CALL is dropped even when
    `LISTEN ON` follows a moment later on the same frequency; the caller's
    next CALL is heard as usual.  The same holds for a host that pauses the
    modem with `LISTEN OFF` and resumes it on the same channel without
    scanning: a CALL on the air across the pause costs that one CALL, and
    the caller's retry is answered.  Up to 1.9.22 such a CALL could still
    decode, on the next channel, and was answered there, where the caller
    could not hear it.  BPQ32 then held the wrong channel for about 2 min
    (six scan intervals) while the caller's repeats went unheard:
    - MFSK: the flush waited for the receive loop to see the station not
      listening, and BPQ32's `LISTEN ON` follows within 1 ms (K7EK,
      2026-11-06: decoded 2.5 s after the hop; 2026-11-07: four calls,
      0.2-4.2 s after it);
    - DATAC16: its decoder was not flushed at all (K7EK, 2026-11-07: two
      calls decoded 1.4 s and 1.9 s after the hop);
    - MFSK, up to 1.9.24: the flush emptied only the audio queue in front of
      the listener, which keeps up to two frames of audio of its own, and
      the next channel's audio completed a CALL cut off near its end (K7EK,
      2026-11-14 and 15: decoded 1.2 s and 4.2 s after a hop that logged
      "drops it"; KG9NI, 2026-11-16: decoded 1.1 s after its scanner came
      back to the channel, 30 s after the CALL, and answered once the caller
      had given up);
  - a `BW` change between `BW500` and `BW2300`/`BW2750` flushes both too: the
    listeners swap floors, and each swap drops what it holds.

  While a scan schedule held KG9NI on one channel (5-31 min), BPQ32 sent no
  hop commands, so the MFSK listeners were not flushed there.
- **Only a whole CALL decoded inside one dwell starts a session.**  CALL
  lengths on the air:

  | CALL | Length | Inside a 10 s dwell |
  |---|---|---|
  | DATAC16 (fast) | 3.74 s | if it starts in the first ~6 s |
  | MFSK | 13.5 s | never |
  | MFSK, BW500 floor | 27 s | never |

  All 10 sessions began with a DATAC16 CALL, decoded 5.7-8.6 s after the
  scanner arrived.  A caller that sends `RETRIES n` gets up to 4 fast CALLs
  before the first MFSK one (see `RETRIES`).  To catch MFSK CALLs, a scanning
  station needs a longer dwell, or a hold that starts before the CALL
  decodes.
- **BPQ32's scanner does not hold on `BUSY ON`**, the only earlier signal
  Mercury sends.  With `busy_detect = true` (KG9NI, 2026-11-06), it kept
  hopping every 10 s through every BUSY spell: 94 hops in one of 16 minutes.
  What BPQ32 does with BUSY is refuse its own outgoing connects ("Sorry, Can't
  Connect - Channel is busy"), and two forwards that night went by AX/IP
  instead.
- **By default one noise floor serves every scanned channel.**  Mercury is
  not told the host retuned, so the busy detector carries its floor from one
  channel to the next.  At KG9NI (2026-11-06) every CLEAR came on one of the
  three channels, and BUSY returned 0.6-5.7 s after the hop to another one,
  on a band the operator heard as empty.  With `busy_reseed_on_hop = true` (or
  [`BUSYRESEED ON`](#busyreseed)) it takes `LISTEN OFF` as the sign of a hop:
  it waits 1 s for the radio to retune, then measures the floor afresh,
  keeping its BUSY/CLEAR verdict until the new channel decides it.
  The trade-off: the floor is taken from the lower-quartile band of the
  300-2700 Hz passband, so a signal wider than about 1.8 kHz (BW2300 DATAC,
  VARA 2300) moves it.  A hop onto a channel where such a signal is already
  under way seeds the floor on it, and the detector misses that signal until
  it stops; the carried floor reports it BUSY.  So the re-seed swaps false
  BUSY on noisier channels for missed BUSY on a hop into a wideband
  transmission, which is why it is the operator's choice.
- **After a host `DISCONNECT`, a scan hop can cut the teardown short.**
  It did up to 1.9.22, when Mercury answered `DISCONNECTED` straight away:
  the scanner's next `LISTEN OFF` released the radio before the peer had the
  DISCONNECT (#473).  On K7EK and KG9NI a DISCONNECT retry due 20 s after the
  first was cut off, and two drains ended with no DISCONNECT sent; each time
  the peer waited out its 240 s watchdog.  BPQ32 keeps the radio until
  `DISCONNECTED`, now sent when the session has closed, restarts its scan
  15 s after it, and sends `ABORT` if it has waited 30 s.
- **After an `ABORT`, BPQ32 does not wait for `DISCONNECTED`.**  It sends
  `ABORT` and releases the port in the same step (linbpq `CheckForDetach`:
  `ForcedClose`, then `CloseComplete`, whose `VARAReleaseTNC` sends
  `LISTEN ON` and `SCANSTART 15`), so its first hop comes 15 s after the
  `ABORT` whatever Mercury answers.  On air (K7EK, 2026-11-10) `LISTEN ON`
  came in the `ABORT`'s millisecond and the hop's `LISTEN OFF` 16.6 s later.
  So a callee's `ABORT`ed DISCONNECT keys out of turn, on a clear channel,
  if no slot comes in that time ([ABORT](#abort)).
- **While BPQ32 calls out, its session has an idle timer.**  A forwarding
  connect script may set it with `IDLETIME`, as low as 60 s, and only the
  TNC lines BPQ32's VARA driver passes on to the session reset it
  (`VARA} BITRATE ...`; it keeps `PTT`, `SN`, `BUSY` and `BUFFER` to
  itself).  On KG9NI (November 2026) calls were dropped 61-62 s after the
  last such line, before the first MFSK CALL had gone out.  Mercury now
  sends a `BITRATE` line on the first CALL and at least every 20 s until the
  call connects or ends (#539; see [BITRATE](#bitrate)).  A session that
  has connected and waits longer than `IDLETIME` for the peer's first data
  is not covered: there BPQ32 passes only data, so raise `IDLETIME` in the
  connect script.

When `LISTEN OFF` arrives with no session up while a CALL decoder has locked
onto a frame it has not decoded yet, Mercury logs it:

```
[tcp-ctl] LISTEN OFF 2.1 s into a DATAC16 frame not yet decoded: LISTEN OFF drops it (or it was noise)
[tcp-ctl] LISTEN OFF 6.4 s into a MFSK frame not yet decoded: LISTEN OFF drops it (or it was noise)
```

Either frame is lost to the `LISTEN OFF` itself.  A CALL cut off by a hop
shows as one of these lines with no `RX_CALL` after it.  The lock can also be noise or another station's
traffic, so read the line together with the caller's log.

#### Calling a scanning gateway

A gateway that scans visits each of its frequencies for a few seconds in
turn: 4 s on an RMS Trimode, 10 to 17 s on BPQ32 scan lists seen on air.
It stops on a frequency when Mercury sends it `PENDING`, which happens about
a second into a CALL on DATAC16 or the 1 kHz MFSK, if the CALL starts while
the gateway is listening there.  A 500 Hz MFSK CALL, at 27 s, never fits a
stay and does not stop the scan: Mercury does not hold a scanner on that
mode's lock, which false-locked on 40 m noise for whole frames.

A caller at BW2300 or BW2750 sends, after its fast DATAC16 CALLs, the 1 kHz
MFSK and then the 500 Hz MFSK twice.  On KG9NI's calls to K7EK (Nov 22,
2026; four channels, 16.7 s each) the two 500 Hz CALLs and the waits after
them took 133 s of the 300 a call lasts, and none could be caught.  With
`arq:call_mfsk500 = false` the CALLs after the fast ones are the 1 kHz MFSK
and DATAC16 in turn, as in 1.9.24:

| CALL | default | `call_mfsk500 = false` |
|---|---|---|
| 1 to `F` | DATAC16 | DATAC16 |
| then, repeating | 1 kHz MFSK, 500 Hz MFSK, 500 Hz MFSK | 1 kHz MFSK, DATAC16 |

The cost is the deepest connects to a station that does not scan: the
500 Hz MFSK reaches about 3 dB further (1.9.25 release notes, #618).  In the
simulator, calling a station that listens on one frequency (40 seeds), every
call at -14 to -16 dB connected with it on and none with it off, and under
fading at -13 dB 39 of 40 against 15.  Turn it off on a station that mostly
calls scanning gateways; leave it on for one that calls fixed stations on
weak paths.  It changes nothing at BW500,
where the 500 Hz MFSK is the floor, nor for the station called.

### DISCONNECTED

Sent when the session ends, either by local DISCONNECT, remote DISCONNECT,
or timeout.

### BUFFER

Sent periodically during data transfer.  The value reflects the number of
application bytes still queued for transmission.  When it reaches `0`, all
data has been acknowledged by the peer.

Above `0` it can run ahead of the acknowledgements.  At the MFSK floor the
receiver answers rounds with short patterns and polls only every few rounds,
so a block's acknowledgement can come minutes after the block is in, or be
lost.  A BREAK pattern ("the block you are on is in") takes that block's
bytes off `BUFFER` at once; the poll that acknowledges it comes later.  A
pattern carries no session, so the block is still held and resent until that
poll; if the poll says it is not in after all, its bytes go back on `BUFFER`.
`0` itself is only ever sent once every byte has been acknowledged: while any
is not, `BUFFER` says at least `1`.

While the peer's data is still on its way and neither data nor a `BUFFER`
above `0` has passed for 50 s, Mercury sends `BUFFER 1` followed at once by
the true count.  BPQ32 closes a Winlink session after about 130 s with neither,
even when an upload is only stalled, and a receiving station has nothing of
its own to report (#392).  Treat a `BUFFER 1` immediately followed by `BUFFER 0`
as no change.

### IAMALIVE

Sent periodically on the control port as a keepalive to detect broken
TCP connections.

### REGISTERED

Sent when a callsign is set via `MYCALL`, confirming the callsign is
licensed (VARA compatibility).  Arrives after the `OK\r` response to
`MYCALL`.  Also sent on client reconnect if a callsign is already set.

`REGISTERED` is **per call sign**, so `MYCALL` emits one line for the primary
and one for each secondary it accepted, in the order given:

```
MYCALL AAAA BBBB CCCC\r
OK\r
REGISTERED AAAA\r
REGISTERED BBBB\r
REGISTERED CCCC\r
```

Secondaries beyond the four Mercury can hold are dropped, and dropped ones are
not acknowledged — the host would otherwise address a callsign this station
never answers.

---

## Data Port

The **data port** (`base_port + 1`, default 8301) carries raw application
payload.  Bytes written to the data port are queued for ARQ transmission;
bytes received from the remote station are delivered on the data port.

No framing is needed — Mercury handles segmentation internally.  The data
port only carries payload when a session is CONNECTED.

Bytes written while no session is up are held in the TX queue (`BUFFER`
counts them) and go out as the first payload of the next session.  A host
may write as soon as it sends `CONNECT`; those bytes are kept for that call.
Bytes are dropped when:

- the data client disconnects while the station is idle or listening (no
  call being placed or answered), with a warning in the log: their writer is
  gone, and they would otherwise reach whatever station connects next;
- the host sends `ABORT`;
- a session ends (bytes it did not carry are not kept for the next one).

A host that keeps its data connection open and must start a session with an
empty queue should check `BUFFER` and send `ABORT` if it is not 0.

While a call is being placed, answered or carried, nothing a host writes is
dropped for lack of room: when the TX queue (64 kB) and the bus in front of
it are full, Mercury stops reading the data port until the session has made
room, and TCP holds the host's writes.  A host may write a whole file at
once.  (With no call up, bytes beyond the queue are not kept: nothing would
send them, and they would be dropped with the rest when the host leaves.)

---

## Broadcast Port

The **broadcast port** (default 8100) is independent of ARQ and uses
**KISS framing**.  One-way broadcast frames are sent/received as
fixed-size KISS-encoded packets matching the modem's payload size.

That frame size follows the **listen mode**, set at startup with `-m` and
changeable at runtime with the [`MODE`](#mode) control-port command.  A
connected broadcast client picks up a new frame size on its next frame — the
socket does not need to be reopened — so a `MODE` change re-frames the plane
underneath a live client.  Size your client's frames from the mode you
selected, not from a cached value.

Three client framings are accepted, distinguished by the KISS command byte:

- `0x00` (standard KISS data / VARA "AX.25 standard") and `0x01` (VARA
  "AX.25 7-char callsign", used by VARA): Mercury injects its 1-byte
  broadcast header plus a 2-byte payload-length prefix, zero-pads to the
  modem frame size, and records which of the two framings the sender used
  in a header extension bit (`BCAST_EXT_KISS_STD`).  The receiving side
  delivers exactly the original payload with the **sender's** command byte
  — so VARA↔VARA traffic stays `0x01` and standard-KISS clients
  (e.g. Reticulum, see [RETICULUM.md](RETICULUM.md)) get `0x00`.
- `0x02` (`CMD_DATA`) is used by two different clients and is distinguished
  by the whole first byte:
  - **hermes-broadcast**: the frame carries a Mercury broadcast header — a
    broadcast packet type (`PACKET_TYPE_BROADCAST_CONTROL`/
    `PACKET_TYPE_BROADCAST_DATA`) **and** a zero extension, which is what
    hermes-broadcast writes for both its config and payload frames.  Passed
    raw; zero-padded if short, discarded if oversized.
  - **VARA beacons/pings** (and any other unformatted `CMD_DATA` payload):
    Mercury wraps it exactly like `0x00`/`0x01` and records the framing in a
    `BCAST_EXT_KISS_DATA` extension bit, so the receiver delivers the original
    payload back as `0x02`.

  Testing only the packet-type bits is not sufficient — they are the top 3
  bits, so 25% of arbitrary first bytes look like a broadcast type, including
  every lowercase letter, and such a beacon would be passed raw and lost.
  Requiring a zero extension as well cuts that to 0.8%, and to nothing across
  `a`–`z`.

  Frame length must **not** be used as the discriminator: hermes-broadcast
  sends its config frames `RQ_HEADER_SIZE` short of a full modem frame and
  relies on Mercury's zero-padding, so a `frame_len == frame_size` test drops
  every config packet and the far side never starts decoding.

Payloads larger than the modem frame (minus 3 bytes of header+length)
are truncated (`0x00`/`0x01`/unformatted-`0x02`) or discarded (raw
hermes-broadcast `0x02`) — the broadcast plane does not fragment.

**Station messages** (packet type `0x07`, the beacon) are the one exception
to the listen mode's frame size.  Sent with `0x03`, they go on the
[`BEACONMODE`](#beaconmode) mode when one is set.  Received, they are handed up
from whichever decoder heard them, at that mode's frame size, whole and with
command `0x03`.  They are never stored as chat.

---

## Typical Session Flow

```
Client                          Mercury
  |                                |
  |--- (TCP connect to 8300) ---->|
  |--- (TCP connect to 8301) ---->|
  |                                |
  |--- MYCALL AAAA\r ------------>|
  |<-- OK\r ----------------------|
  |<-- REGISTERED AAAA\r ---------|
  |                                |
  |--- LISTEN ON\r -------------->|
  |<-- OK\r ----------------------|
  |                                |
  |--- BW2300\r ------------------>|
  |<-- OK\r ----------------------|
  |                                |
  |--- CONNECT AAAA BBBB\r ------>|
  |<-- OK\r ----------------------|
  |                                |  (RF: CALL/ACCEPT exchange)
  |<-- CONNECTED AAAA BBBB 2300\r |
  |                                |
  |--- (write data to 8301) ----->|  (RF: DATA frames)
  |<-- BUFFER 2404\r -------------|
  |<-- PTT ON\r ------------------|
  |<-- PTT OFF\r ------------------|
  |<-- BUFFER 0\r ----------------|
  |                                |
  |<-- (read data from 8301) -----|  (received from remote)
  |                                |
  |--- DISCONNECT\r -------------->|
  |<-- OK\r ----------------------|
  |<-- DISCONNECTED\r ------------|
```

---

## See Also

- [The carousel data plane](CAROUSEL-ARQ.md) — the ARQ data link and its wire formats.
- [ARQ Protocol & Architecture](ARQ.md) — the connection FSM; mostly the history of the 1.9.x data plane.
- [Mercury README](../README.md) — Build instructions, CLI usage, project links.
