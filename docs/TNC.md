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

All control commands and responses are **CR-terminated ASCII** (`\r`).

---

## Commands (Client → Mercury)

Commands are sent on the **control port**.

### MYCALL

Set the local station callsign (up to 15 characters).

```
MYCALL <callsign>\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

Must be set before `LISTEN ON` or `CONNECT`.

---

### LISTEN

Enable or disable listening for incoming ARQ connections.

```
LISTEN ON\r
LISTEN OFF\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

When enabled, Mercury enters the LISTENING state and will accept incoming
CALL frames addressed to the local callsign (or any callsign if PUBLIC is ON).

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

- **BW2300** — Full bandwidth.  Allows gear-shifting up to DATAC1 (510 bytes/frame).
- **BW500** — Narrow bandwidth.  Restricts the maximum payload mode to DATAC3/DATAC4.
- **BW2750** — Accepted for compatibility, but currently behaves like **BW2300**.

---

### COMPRESSION

No-op for VARA client compatibility.

```
COMPRESSION ON\r
COMPRESSION OFF\r
```

**Response:** `OK\r` (always).

Mercury does not use compression; this command exists so VARA-compatible
clients (e.g., Pat, VarAC) can connect without errors.

---

### CHAT

Enable chat-optimized mode (VARA compatibility).

```
CHAT ON\r
CHAT OFF\r
```

**Response:** `OK\r` on success, `WRONG\r` on error.

`CHAT ON` implicitly enables `LISTEN ON`, placing Mercury in the LISTENING
state.  This matches VARA behavior where chat applications (VarAC, VARA Chat)
expect the modem to be ready for incoming connections after `CHAT ON`.

`CHAT OFF` is acknowledged but has no effect — Mercury does not currently
differentiate chat and file-transfer timing.

---

### P2P

No-op for VARA client compatibility.

```
P2P\r
```

**Response:** `OK\r` (always).

---

### CONNECT

Initiate an ARQ connection to a remote station.

```
CONNECT <mycall> <theircall>\r
```

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error.

Mercury will transmit CALL frames on DATAC13 and wait for an ACCEPT.
On success, the asynchronous response `CONNECTED <mycall> <theircall> <bandwidth>\r`
is sent on the control port.

Example:
```
CONNECT AAAA BBBB\r
```

---

### DISCONNECT

Terminate the current ARQ session.

```
DISCONNECT\r
```

**Response:** `OK\r` if the command was accepted, `WRONG\r` on error.

Mercury sends DISCONNECT frames to the peer.  Once complete, the
asynchronous response `DISCONNECTED\r` is sent on the control port.

---

### BUFFER

Query the number of bytes pending in the ARQ transmit buffer.

```
BUFFER\r
```

**Response:** `BUFFER <bytes>\r` on the control port.

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

**Response:** `BITRATE (<speed_level>) <bps> BPS\r` on the control port.

---

### RETRIES

Override the amount of retries Mercury will try to connect or retry a transmission before giving up.
Setting to 0 should revert to the defined values in datalink_arq/arq_protocol.h.

```
RETRIES 10\r
```

---


## Asynchronous Responses (Mercury → Client)

These are sent on the **control port** without a preceding command.

| Response                                    | Meaning                                      |
|---------------------------------------------|----------------------------------------------|
| `CONNECTED <mycall> <theircall> <bandwidth>\r` | ARQ session established                   |
| `DISCONNECTED\r`                            | ARQ session ended                            |
| `PTT ON\r`                                  | Radio transmitter keyed                      |
| `PTT OFF\r`                                 | Radio transmitter unkeyed                    |
| `BUFFER <bytes>\r`                          | TX buffer level update (periodic)            |
| `SN <value>\r`                              | SNR update                                   |
| `BITRATE (<level>) <bps> BPS\r`            | Throughput update                            |
| `IAMALIVE\r`                                | Heartbeat (sent periodically while idle)     |

### CONNECTED

Sent when a session is successfully established (either outgoing CONNECT
or incoming CALL accepted).  The `<bandwidth>` value is the effective
negotiated bandwidth, currently `500` or `2300`.

### DISCONNECTED

Sent when the session ends, either by local DISCONNECT, remote DISCONNECT,
or timeout.

### BUFFER

Sent periodically during data transfer.  The value reflects the number of
application bytes still queued for transmission.  When it reaches `0`, all
data has been acknowledged by the peer.

### IAMALIVE

Sent periodically on the control port as a keepalive to detect broken
TCP connections.

---

## Data Port

The **data port** (`base_port + 1`, default 8301) carries raw application
payload.  Bytes written to the data port are queued for ARQ transmission;
bytes received from the remote station are delivered on the data port.

No framing is needed — Mercury handles segmentation internally.  The data
port only carries payload when a session is CONNECTED.

---

## Broadcast Port

The **broadcast port** (default 8100) is independent of ARQ and uses
**KISS framing**.  One-way broadcast frames are sent/received as
fixed-size KISS-encoded packets matching the modem's payload size.

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

- [ARQ Protocol & Architecture](ARQ.md) — Wire protocol, FSM, gear-shifting, tuning guide.
- [Mercury README](../README.md) — Build instructions, CLI usage, project links.
