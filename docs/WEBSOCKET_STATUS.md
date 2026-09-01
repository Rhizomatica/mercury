# WebSocket status telemetry

Mercury's `status` message preserves its established field order. New fields
are appended after the audio-health fields:

- `arq_tx_mode`: local ARQ payload mode (`DATAC1`, `DATAC3`, `DATAC4`,
  `DATAC15`, `DATAC17`, or `QAM16C2`), or an empty string when unavailable.
- `arq_rx_mode`: the peer payload mode used by the local decoder, independently
  reported from the transmit mode.
- `radio_frequency_hz`: the last successful read through Mercury's existing
  Hamlib session, or `null` when unavailable.
- `radio_frequency_age_ms`: age of that cached frequency, or `null`.

Frequency telemetry is read-only. It never opens another CAT session, changes
the VFO, or waits for the shared radio mutex. A busy radio causes the optional
refresh to be skipped and the existing cache to be returned. Polls are also
suppressed during an ARQ connection, while transmitting, and for two seconds
after PTT release. Mercury logs each CAT read duration so slow real-world rig
backends can be identified without putting timing guesses into the wire API.
