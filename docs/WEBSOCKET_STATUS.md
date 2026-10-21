# WebSocket status telemetry

Mercury's `status` message preserves its established field order. New fields
are appended after the audio-health fields:

- `arq_tx_mode`: local ARQ payload mode (`MFSK`, `DATAC15`, `DATAC4`,
  `DATAC3`, `DATAC1`, `DATAC17`, or `QAM16C2`), or an empty string when
  unavailable.  In a carousel session it is the mode of this station's last
  data round, and empty until it has sent one: a station that only polls has
  no transmit mode.
- `arq_rx_mode`: the peer payload mode, independently reported from the
  transmit mode.  In a carousel session, the mode of the last data frame
  received, and empty until one has arrived.
- `radio_frequency_hz`: the last successful read through Mercury's existing
  Hamlib session, or `null` when unavailable.
- `radio_frequency_age_ms`: age of that cached frequency, or `null`.
- `peer_snr`, `peer_snr_valid`: kept for compatibility.  The carousel
  carries no dB value for our signal, so `peer_snr_valid` is always false and
  `peer_snr` the -99.9 sentinel; `peer_hears_mode` reports what the peer
  hears instead.
- `peer_hears_mode`: in a carousel session, the highest rung the far station
  last reported its SNR supports for our signal (`MFSK` ... `QAM16C2`, or
  at most `DATAC3` in a BW500 session), from
  its ACCEPT or its polls; empty otherwise, and empty while it has not
  reported (a station that only receives gets no polls from its sender).
- `freq_offset_hz`: the carrier offset the last frame decoded arrived at, in
  Hz, positive when above nominal; `null` until a frame has been decoded.
  Every decoder reports it (MFSK to an eighth of a subcarrier, 3.9 Hz; the
  OFDM modes their tracked estimate) except the monitor decoders, which hear
  other stations' traffic.  It is the far radio's dial error as we hear it,
  plus whatever a sample-clock error moves 1500 Hz by: a sound card 1.5 %
  slow shows as +23 Hz.
- `freq_offset_age_ms`: how long ago that frame was decoded, or `null`.
- `listen_mode`, `listen_mode_name`: the broadcast plane's listen mode, the
  payload mode the station sits on while no ARQ session is up (the `MODE`
  index and its name; `-1` and empty while unknown).
- `busy`: what holds the modem now, so a client can see it before it asks:
  `"arq"` (a session up or starting), `"broadcast"` (the host's carousel
  sending), `""` when free.  `MODE` answers `BUSY` in both.
- `broadcast`: the host's broadcast carousel (`mercury_host_bcast_send`):
  `state` (`idle`, `sending`, `stopped_by_arq`, `stopped`, `done`, `error`),
  `cycle` (the pass now on), `cycles_total` (`0`: until stopped) and
  `frames_sent`.  An ARQ session starting stops it (`stopped_by_arq`).

Frequency telemetry is read-only. It never opens another CAT session, changes
the VFO, or waits for the shared radio mutex. A busy radio causes the optional
refresh to be skipped and the existing cache to be returned. Polls are also
suppressed during an ARQ connection, while transmitting, and for two seconds
after PTT release. Mercury logs each CAT read duration so slow real-world rig
backends can be identified without putting timing guesses into the wire API.
