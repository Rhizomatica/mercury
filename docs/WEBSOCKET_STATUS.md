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
- `peer_snr`, `peer_snr_valid`: the SNR the far station reports for our
  signal; `peer_snr_valid` is false (and `peer_snr` the -99.9 sentinel) until
  a report arrives.  The carousel carries no dB value, so in a carousel
  session these stay unset.
- `peer_hears_mode`: in a carousel session, the highest rung the far station
  last reported its SNR supports for our signal (`MFSK` ... `QAM16C2`), from
  its ACCEPT or its polls; empty otherwise, and empty while it has not
  reported (a station that only receives gets no polls from its sender).

Frequency telemetry is read-only. It never opens another CAT session, changes
the VFO, or waits for the shared radio mutex. A busy radio causes the optional
refresh to be skipped and the existing cache to be returned. Polls are also
suppressed during an ARQ connection, while transmitting, and for two seconds
after PTT release. Mercury logs each CAT read duration so slow real-world rig
backends can be identified without putting timing guesses into the wire API.
