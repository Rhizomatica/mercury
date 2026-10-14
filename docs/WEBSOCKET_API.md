# Mercury WebSocket API

Mercury exposes a single bidirectional WebSocket endpoint used by the Qt client,
the embedded and published web UIs, and the Fyne client. It doubles as a small
static HTTP server for the bundled web pages.

- **Default URL:** `ws://<host>:10000/websocket` (plain) or
  `wss://<host>:10000/websocket` (TLS, negotiated at connect).
- **Direction:** client → server **commands**, server → client **events** and
  **binary spectrum frames**, plus a per-command acknowledgement.
- **Encoding:** commands and events are JSON **text** frames; the waterfall is
  a **binary** frame. Frames from the client are always masked (RFC 6455).

The same command handler serves the embedded (in-process) Fyne UI, so local and
remote clients cannot diverge in behaviour.

---

## 1. Transport and connection

| Property | Value | Source |
|---|---|---|
| Endpoint path | `/websocket` | `gui_interface/websocket/mercury_websocket.c:138` |
| Default port | `10000` (`UI_DEFAULT_PORT`) | `gui_interface/ui_communication.h:37` |
| Bind address | `0.0.0.0` | `mercury_websocket.c:137` |
| Plain / TLS | `ws` or `wss` (build must have OpenSSL) | `mercury.ini.example:20-23` |
| Max concurrent connections | 32 | `ws_server.c:50` |
| Max inbound message | 8192 bytes (`WS_MAX_MESSAGE_SIZE`) | `gui_interface/websocket/mercury_websocket.h:24` |
| TLS certificate / key | PEM files (`ui_tls_cert` / `ui_tls_key`) | `mercury.ini.example:25-29` |

Incoming **binary** frames from a client are ignored — clients only send text
(`mercury_websocket.c:53-54`).

### HTTP behaviour on the same port

The server also answers HTTP so a browser can load the UI. It accepts
`GET` and `HEAD` only.

| Response | When |
|---|---|
| `101 Switching Protocols` | Valid WebSocket upgrade |
| `200 OK` | Static asset fetch (`/` → `index.html`, etc.) |
| `400 Bad Request` | Malformed request / missing `Upgrade: websocket` / missing `Sec-WebSocket-Key` / bad path |
| `404 Not Found` | Unknown asset |
| `405 Method Not Allowed` | Method other than `GET`/`HEAD` (`Allow: GET, HEAD`) |
| `426 Upgrade Required` | Missing/invalid `Upgrade` or `Sec-WebSocket-Version: 13` |
| `431 Request Header Fields Too Large` | Oversized request header block |

Static pages are compiled into the binary (`gui_interface/websocket/web_assets.h`,
`web_packed.c`): `index.html`, `mercury.html`, `waterfall.html`, `test.html`.

---

## 2. Command envelope (client → server)

Every command is a flat JSON object. The `command` key is **mandatory**; a
missing `command` yields `{"error":"malformed command JSON"}`. Values are
carried in positional string fields `value` … `value7` (all optional, default
empty).

```json
{
  "command": "set_audio_config",
  "value":   "plughw:0,0",
  "value2":  "plughw:0,0",
  "value3":  "left",
  "value4":  "alsa",
  "value5":  "",
  "value6":  "",
  "value7":  ""
}
```

Struct definition: `ws_command_t` in `gui_interface/websocket/ws_json.h:23-32`.
Parsing: `ws_json_parse_command()` (`ws_json.c:307-327`), a structural flat-JSON
walker (`ws_json_find_key()`, `ws_json.c:233-305`) that decodes JSON escapes
including `\uXXXX` and surrogate pairs. A malformed object returns -1.

Limits: `command` ≤ 63 chars, each `valueN` ≤ 255 chars; the whole message must
be ≤ 8192 bytes. Unknown commands log a warning and return an error.

---

## 3. Commands

There are exactly five implemented commands. Dispatch is a `strcmp` chain in
`ui_comm_handle_command()` (`gui_interface/ui_communication.c:162-367`).

### 3.1 `set_audio_config`

Change the audio subsystem, capture/playback devices and input channel.
Restarts `audioio`, waits up to 3 s for health, and persists to the INI on
success.

| Field | Meaning | Accepted values |
|---|---|---|
| `value` | Capture (input) device id | Device id (e.g. `plughw:0,0`); empty = leave current |
| `value2` | Playback (output) device id | Device id; empty = leave current |
| `value3` | Input channel | `left` (default), `right`, `stereo` |
| `value4` | Audio subsystem (optional) | `alsa`, `pulse`, `dsound`, `wasapi`, `oss`, `coreaudio`, `aaudio`, `shm`, `null`, `fifo`, `sock`, `rtp` |

If the subsystem changes and no device is supplied for it, the stale device id
is cleared so the new subsystem resolves its own default. Invalid subsystem →
error. Source: `ui_communication.c:171-277`.

### 3.2 `set_ptt_config`

Configure PTT (rig control). Applies via `ui_apply_ptt_config`, persists, and
republishes the radio list.

| Field | Meaning | Accepted values |
|---|---|---|
| `value` | PTT method | `none`, `hamlib`, `serial`, `cm108`, `hermes_shm` (legacy `serial_rts`) |
| `value2` | Device path | e.g. `/dev/ttyUSB0`, `127.0.0.1:4532` |
| `value3` | Hamlib model id | Integer > 0 (required for `hamlib`) |
| `value4` | Hamlib serial speed | Integer, clamped ≥ 0 |
| `value5` | Serial PTT line | `rts`, `dtr`, `both` |
| `value6` | Serial invert | `none`, `rts`, `dtr`, `both` |
| `value7` | CM108 GPIO pin | Integer 1–4 |

Validation: `hamlib` requires `value3 > 0`; `serial` requires a non-empty
`value2`. Omitted advanced fields (`value3`–`value7`) retain their current
values. Source: `ui_communication.c:278-326`.

### 3.3 `set_waterfall`

Enable or disable the spectrum / waterfall feed.

| Field | Meaning |
|---|---|
| `value` | `"off"` disables; **any other value enables** |

Toggles `modem_set_spectrum_enabled`, starts/stops the spectrum publisher
thread, and persists `waterfall_enabled`. Source: `ui_communication.c:328-331`,
`ui_communication.c:369-403`.

### 3.4 `set_tx_gain`

Set the transmit gain.

| Field | Meaning |
|---|---|
| `value` | TX gain in dB as a string; `atof`, NaN/Inf → 0, clamped to **[-20, +20] dB** |

The dB value is converted to linear (`10^(dB/20)`) and pushed via
`modem_set_tx_gain`, then persisted to the INI. Source:
`ui_communication.c:333-348`.

### 3.5 `set_callsign`

Persist the operator callsign so it survives a restart.

| Field | Meaning |
|---|---|
| `value` | Operator callsign (stored in `cfg.callsign`) |

Source: `ui_communication.c:350-359`.

---

## 4. Command replies (server → same connection)

Sent on the connection that sent the command (`mercury_websocket.c:45-102`).

| Condition | Response |
|---|---|
| Handler returned 0 | `{"status":"ok"}` |
| Handler returned non-zero `rc` | `{"status":"error","code":<rc>}` |
| No command handler registered | `{"error":"no handler"}` |
| Empty message or > 8192 bytes | `{"error":"invalid message size"}` |
| JSON parse failure / missing `command` | `{"error":"malformed command JSON"}` |

---

## 5. Events (server → all clients)

Sent to every connected client via `ws_broadcast_json()` /
`ws_broadcast_binary()`.

| Event (`type`) | Encoding | Cadence / trigger |
|---|---|---|
| `status` | text JSON | Every 500 ms (`UI_PUBLISH_INTERVAL_US`) |
| `capture_dev_list` | text JSON | On connect / after an audio change |
| `playback_dev_list` | text JSON | On connect / after an audio change |
| `input_channel` | text JSON | On connect / after an audio change |
| `audio_system` | text JSON | On connect / after an audio change |
| `radio_list` | text JSON | On connect / after a PTT change |
| `history` | text JSON | On connect |
| spectrum | **binary** | Every 50 ms (~20 fps), when waterfall enabled |

### 5.1 `status`

Rendered by `ui_status_to_json()` (`gui_interface/ui_status.c:11-84`). Field
order is a stable wire format — do not reorder.

```json
{"type":"status",
 "bitrate":0,
 "snr":0.0,
 "user_callsign":"NOCALL",
 "dest_callsign":"",
 "sync":false,
 "direction":"rx",
 "client_tcp_connected":false,
 "bytes_transmitted":0,
 "bytes_received":0,
 "tx_gain_db":0.0,
 "tx_peak_dbfs":0.0,
 "waterfall":true,
 "audio_ok":true,
 "audio_error":"",
 "arq_tx_mode":"",
 "arq_rx_mode":"",
 "radio_frequency_hz":null,
 "radio_frequency_age_ms":null,
 "peer_snr":-99.9,
 "peer_snr_valid":false,
 "peer_hears_mode":""}
```

| Field | Notes |
|---|---|
| `bitrate` | Bits per second |
| `snr` | Signal-to-noise ratio (dB, 1 decimal) |
| `user_callsign` / `dest_callsign` | Local and destination callsigns |
| `sync` | ARQ/modem sync flag |
| `direction` | `"tx"` or `"rx"` |
| `client_tcp_connected` | TNC TCP client connected |
| `bytes_transmitted` / `bytes_received` | Counters |
| `tx_gain_db` / `tx_peak_dbfs` | TX gain and peak level |
| `waterfall` | Spectrum feed enabled |
| `audio_ok` / `audio_error` | Audio health and reason string |
| `arq_tx_mode` / `arq_rx_mode` | Payload modes; see `WEBSOCKET_STATUS.md` |
| `radio_frequency_hz` / `radio_frequency_age_ms` | Cached Hamlib frequency, or `null` |
| `peer_snr` / `peer_snr_valid` | Kept for compatibility (`-99.9` / `false`) |
| `peer_hears_mode` | Highest rung the peer reports supporting |

See [WEBSOCKET_STATUS.md](WEBSOCKET_STATUS.md) for the full semantics of the
ARQ-mode, frequency and peer fields.

### 5.2 `capture_dev_list` / `playback_dev_list`

Built by `ui_device_list_to_json()` (`ui_status.c:86-110`):

```json
{"type":"capture_dev_list",
 "selected":"plughw:0,0",
 "list":[{"name":"Built-in Audio","id":"plughw:0,0"}]}
```

`type` is `capture_dev_list` or `playback_dev_list`.

### 5.3 `input_channel`

```json
{"type":"input_channel",
 "selected":"left",
 "list":["left","right","stereo"]}
```

Source: `ui_communication.c:796-800`.

### 5.4 `audio_system`

```json
{"type":"audio_system",
 "selected":"alsa",
 "list":["alsa","pulse"]}
```

`list` contains only the subsystems compiled into this build. Source:
`ui_communication.c:802-818`.

### 5.5 `radio_list`

```json
{"type":"radio_list",
 "selected":"",
 "device_path":"/dev/ttyUSB0",
 "serial_speed":9600,
 "ptt_method":"serial",
 "ptt_line":"rts",
 "ptt_invert":"none",
 "cm108_gpio":1,
 "list":[{"name":"None","id":"0"}]}
```

`list` is the Hamlib model list with `None` first. Source:
`ui_communication.c:822-868`.

### 5.6 `history`

Wraps the persisted chat ring (each line is a complete JSONL object) into a
single frame (`ui_history.c:26-50`):

```json
{"type":"history",
 "messages":[
   {"ts":1700000000,"ms":123,"plane":"arq","dir":"rx",
    "peer":"MYCALL","text":"hello"}
 ]}
```

Message fields: `ts` (epoch seconds), `ms` (milliseconds), `plane`
(`arq`|`bcast`), `dir` (`tx`|`rx`), `peer`, `text`. Source:
`common/message_store.c:411-414`.

### 5.7 Binary spectrum / waterfall frame

Sent at ~20 fps while the waterfall is enabled
(`ui_communication.c:909-962`). All multi-byte fields are **little-endian**.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic, `uint32` = `0x4D435259` (`"YCRM"`) |
| 4 | 2 | FFT size (`uint16`), number of bins |
| 6 | 2 | Sample rate (`uint16`) |
| 8 | `fft_size × 4` | `float32` dB bins |

Total length = `8 + fft_size * 4`.

---

## 6. WebSocket-level behaviour

Handled by `gui_interface/websocket/ws_frame.c` and `ws_server.c`. Opcodes
(`ws_frame.h:22-27`): `CONT 0x0`, `TEXT 0x1`, `BINARY 0x2`, `CLOSE 0x8`,
`PING 0x9`, `PONG 0xA`.

- Fragmented messages are reassembled (continuation frames supported).
- `Ping` → `Pong` is answered automatically.
- `Close` is echoed back; reserved codes `1005`, `1006`, `1015` are normalised.
- Protocol violations close the connection with **1002** (unknown opcode,
  continuation without a start frame, interleaved fragments, unmasked client
  frame).
- A message larger than the limit closes with **1009**.
- Server-to-client frames are never masked (RFC 6455).

---

## 7. Configuration

INI (`mercury.ini.example:11-32`):

| Key | Default | Meaning |
|---|---|---|
| `ui_enabled` | `false` | Enable the WebSocket server |
| `ui_port` | `10000` | Listen port |
| `ui_protocol` | `ws` | `ws` (plain) or `wss` (TLS) |
| `ui_tls_cert` | — | PEM certificate for `wss` |
| `ui_tls_key` | — | PEM private key for `wss` |
| `waterfall_enabled` | `true` | Start the spectrum feed |

CLI flags: `-G` enable UI WebSocket, `-U <port>` set the port, `-T` enable
WSS/TLS (see `mercury.1`).

---

## 8. Example session

```text
# Enable waterfall and set a callsign
{"command":"set_waterfall","value":"on"}          -> {"status":"ok"}
{"command":"set_callsign","value":"N0CALL"}       -> {"status":"ok"}

# Set 10 dB TX gain
{"command":"set_tx_gain","value":"10"}            -> {"status":"ok"}

# Bad gain (rejected by validation)
{"command":"set_ptt_config","value":"hamlib"}     -> {"status":"error","code":-1}

# Unknown command
{"command":"frobnicate"}                          -> {"status":"error","code":-1}
```

---

## 9. Source map

| File | Role |
|---|---|
| `gui_interface/ui_communication.c` | Command dispatcher (`ui_comm_handle_command`) and event publishers |
| `gui_interface/websocket/mercury_websocket.c` | Adapter: JSON parse, ack/error replies, broadcast API |
| `gui_interface/websocket/mercury_websocket.h` | Public WS API and `WS_MAX_MESSAGE_SIZE` |
| `gui_interface/websocket/ws_server.c` | Poll loop, handshake, framing, HTTP, broadcast queue |
| `gui_interface/websocket/ws_server.h` | Server options and callbacks |
| `gui_interface/websocket/ws_frame.c` / `.h` | RFC 6455 frame codec |
| `gui_interface/websocket/ws_json.c` / `.h` | Flat-JSON parser, `ws_command_t`, UTF-8 sanitiser |
| `gui_interface/websocket/ws_crypto.c` / `.h` | SHA-1 + base64 for `Sec-WebSocket-Accept` |
| `gui_interface/websocket/ws_tls.c` / `.h` | Optional WSS via OpenSSL |
| `gui_interface/ui_status.c` | `status` and device-list JSON rendering |
| `gui_interface/ui_history.c` | `history` event rendering |
| `common/message_store.c` | Chat history ring and JSONL message format |
