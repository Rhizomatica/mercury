# loopsim — two-instance ALSA-loopback ARQ testbed

Runs **two real mercury instances** on one host, connected through the kernel
ALSA `snd-aloop` loopback, with a per-direction AWGN `noisebridge` in each path:

```
A.play --aloop--> [fwd noise] --aloop--> B.capture
B.play --aloop--> [rev noise] --aloop--> A.capture
```

Unlike the FIFO `ch`/watterson bridge (which paces audio in userspace and is
fragile under load), the kernel clocks the loopback at the sample rate, so this
is a faithful, stable, **bidirectional** ARQ testbed with the full FSM/timing
context — ideal for the **asymmetric-link** case (clean forward, noisy reverse)
that hurts ACK survival on real HF.

## Why two instances can coexist
The ALSA/PULSE audio backends use in-process buffers (not the fixed-name
`/signal-*` cross-process SHM), so multiple mercury processes don't collide.
This needs a mercury built from a tree with that change (mercuryv2 and later).

## Prereqs
```
sudo modprobe snd-aloop      # creates "card N: Loopback"
make                         # builds noisebridge (in this dir)
make -C ../.. mercury        # or build mercury however you normally do
```

## Run
```
# clean channel:
./run_loopsim.sh                      # FWD=0 REV=0
PAYLOAD=5120 python3 drive.py         # transfer 5 KB A->B, report bytes/bps/match

# asymmetric (clean forward, noisy reverse ~ the OTA ACK-survival case):
./run_loopsim.sh 0.0 1.0              # rev noise ~ -4 dB: completes
./run_loopsim.sh 0.0 2.0              # rev noise harsher: stalls (ACKs fail)
PAYLOAD=5120 python3 drive.py
```
Env: `MERCURY=./mercury CARD=2` override the binary path / loopback card index.
Mercury logs: `/tmp/mA.log` (caller), `/tmp/mB.log` (listener).

## A/B two builds
Point `MERCURY` at each build in turn (same noise levels) and compare the
`drive.py` RESULT line and the `[arq-timing]`/`OLLA-state` lines in the logs.

## Cleanup
```
pkill -9 -x mercury; pkill -9 -f noisebridge; pkill -9 -f 'arecord -D plughw'
```
