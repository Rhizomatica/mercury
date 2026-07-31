# Windowed ARQ — dummy-load test plan (estacao2 ↔ estacao3)

## Why this run exists

Everything in the windowed leap is proven **deterministically** — unit tests,
the two-FSM sim, and the virtual-clock `-x sock` transport. What none of those
contain is a radio:

| proven off-air | only a radio shows |
|---|---|
| window/SACK/retransmit logic | codec2 OFDM burst boundary re-sync on a real RX |
| ACK epoch tagging, piggyback | audio backend buffering, capture jitter, PTT/relay timing |
| ladder decisions from outcomes | real SNR, real fades, real asymmetry |
| goodput in signal time | thermal/CPU behaviour over a long transfer |

Two of this branch's worst bugs (the fixed-K>1 crawl and the fast-ACK stall)
were **invisible to the sims and only appeared on a live modem**. Assume that
class is not exhausted.

## What is under test

Three builds, deployed one at a time to **both** stations:

| label | branch | what it is |
|---|---|---|
| `trunk` | `mercuryv2` | pre-windowing stop-and-wait — the reference |
| `rethink` | `mfsk-arq-integration` | delivery-driven stop-and-wait + pattern ACK, no windowing |
| `windowed` | `arq-windowed` | the above + K>1 bursts, block packing, adaptive depth, window 128, fast windowed ACK, piggyback ACK |

Fast-ACK and piggyback are **default-on** in `windowed`; no env needed.
Running all three isolates the two contributions instead of conflating them.

## Stations

- **estacao2** = `192.168.10.234` = `PU2UIT-2` — Raspberry Pi 5
- **estacao3** = `192.168.10.106` = `PU2UIT-3` — Raspberry Pi 4
- SSH: user `pi`, password `hermes`. Force password auth and go **serial** —
  parallel SSH trips sshd MaxStartups and hangs.
- Mercury runs from `/root/mercury` as root, logging to `manual.log`.
- Mercury **must** run with `-S` (sbitx SHM keying) or it never keys the radio.

## Rules that have cost us runs before

1. **Never `systemctl restart sbitx` between runs.** It drifts the radio
   gain/AGC and the next run measures a different link (deaf RX, ~2.7 dB SNR,
   failed connects). Only ever swap the mercury binary.
2. **One mercury per station.** `pgrep -c mercury` must be 1 before a run.
3. **Never put `mercury` in the argv of a shell that also runs `pkill`** — the
   pkill matches its own command line and kills the run. Always drive from a
   script file.
4. **Clear stale uucp jobs** (`uustat -a`; kill with `uustat -k <jobid>`) or the
   timed transfer silently includes backlog.
5. `uucpd.service` + `uucp.socket` re-establish LISTEN on the receiver, so after
   relaunching mercury do **not** issue LISTEN by hand — just wait ~14 s.

## Pre-flight (do this before trusting any number)

1. **Link symmetry.** The bench has shown a bad asymmetry — one direction ~0 dB,
   the other ~12 dB — and ARQ needs *both* the data path and the ACK path, so an
   asymmetric link caps both directions. Run one short transfer each way and read
   the receiver's SNR:
   ```
   sudo grep -aoE "snr=[-0-9.]+ sync=[0-9]" /root/mercury/manual.log | tail
   ```
   If the two directions differ by more than a few dB, fix the rig (TX drive / RX
   gain) before measuring anything. That is hardware, not software.
2. **CPU headroom**, especially the Pi 4 receiver: `top -bn1 | head -15`. RX
   should sit ~10–15%. A pegged core makes every timing number meaningless.
3. **Thermals.** A 32 KB bulk transfer keys the PA for minutes. Check the sbitx
   PA temperature between bulk runs and give it time to cool — a hot PA changes
   output power, which changes the link, which changes the result.
4. **Disk**: `df -h /root` — a full log partition truncates the evidence you are
   about to need.

## The measurement protocol

**Interleave, do not block.** The dummy-load link drifts over tens of minutes.
Running all of `trunk` then all of `windowed` bakes that drift into the
comparison. Run `trunk, rethink, windowed, trunk, rethink, windowed, …` so drift
hits every arm equally. `utils/dummyload_ab.sh` does this.

**Payloads** (already on the stations):
- `/root/test_file` — 5632 B, the chatty/handshake-dominated case
- `/root/test_image.png` — 9254 B
- a 32 KB file for the bulk case (the script creates one if absent)

**Metrics**, per run:
- `uucico -S` wall time — the headline (connect + data + teardown)
- from `manual.log`: `[TMG] [arq-timing] disconnect reason=… tx_bytes=N
  rx_bytes=M frames_tx=… frames_rx=… retries=…`
- keydown count and turnarounds — where the windowed leap is supposed to show

**Reps:** 3 per build per payload minimum. The bench is noisy; a single pair of
numbers proves nothing and has misled us before.

## What to watch for specifically

Beyond raw speed, these are the failure modes this branch can plausibly have:

- **Window over-retirement wedge** (fixed once, worth confirming): a tagged or
  implicit ACK retiring the whole window while a capped retransmit re-sends only
  a subset → the un-resent tail is discarded and the transfer stalls. Symptom: a
  transfer that stops advancing with no retries logged.
- **Fast-ACK epoch mismatch:** ACKs accepted for the wrong window generation —
  shows as data being retired that was never delivered (final file corrupt
  despite a clean finish).
- **Adaptive depth thrash:** burst depth oscillating instead of settling.
- **MFSK floor untouched:** at the fringe the ladder must sit at MFSK with
  window == 1. Reach the fringe by lowering TX gain (`set_tx_gain`, or
  `tx_gain_db` in `mercury.ini`) rather than by changing the antenna path — it
  is repeatable and does not touch the AGC.
- **Bidirectional/uucp turnaround:** piggyback ACK should cut the handshake
  reversals; `uucp` protocol `y` is the case it was built for.

## When something goes wrong

Capture, before restarting anything:
```
sudo cp /root/mercury/manual.log /root/mercury/manual-$(date +%s).log
sudo grep -aE "TMG|arq-timing|OLLA-state|window|SACK|epoch" /root/mercury/manual.log | tail -200
uustat -a
```
A run that fails with the log already rotated away is a run you have to do again.

## Running it

```
# from the dev machine, both stations reachable:
utils/dummyload_ab.sh --builds trunk,rethink,windowed --reps 3 \
                      --payload /root/test_file --out results-bench.csv
```
The script deploys each build to both stations, waits for uucpd to re-attach,
clears stale jobs, runs the timed transfer, pulls the timing line, and appends a
CSV row per run. It refuses to start if any pre-flight check fails.

## Reporting

A result is `build, payload, rep, wall_s, tx_bytes, frames_tx, retries, snr`.
Report medians, not means (one failed connect skews a mean badly), and quote the
spread. If `windowed` does not beat `rethink` on the bench, say so — the sim and
`-x sock` gains are real but they are gains in *turnaround*, and a link whose
bottleneck is the modem ceiling or a bad ACK path will not show them.
