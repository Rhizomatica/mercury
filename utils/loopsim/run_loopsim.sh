#!/bin/bash
# run_loopsim.sh — two mercury instances over an ALSA snd-aloop "channel".
#
# Wires two mercury processes (A, B) through the kernel ALSA loopback, with a
# noisebridge injecting a controlled per-direction AWGN level into each path:
#
#     A.play --aloop--> [fwd noisebridge] --aloop--> B.capture
#     B.play --aloop--> [rev noisebridge] --aloop--> A.capture
#
# This is a faithful, KERNEL-PACED bidirectional ARQ testbed (no FIFO-bridge
# real-time-pacing fragility) and reproduces the asymmetric-link ACK behaviour
# (clean forward, noisy reverse).  Needs the snd-aloop module loaded and a
# mercury built with the SHM-decoupling (ALSA backend uses in-process buffers,
# so two instances can coexist).
#
# Prereqs:
#   sudo modprobe snd-aloop            # provides "card N: Loopback"
#   make -C utils/loopsim              # builds noisebridge
#
# Usage: run_loopsim.sh [FWD_NOISE] [REV_NOISE]
#   FWD_NOISE / REV_NOISE : noisebridge stddev fraction (default 0.0 = clean).
#                           ~1.0 ≈ -4 dB SNR; ~2.0 stalls a marginal ACK path.
# Env overrides: MERCURY=./mercury  CARD=2  (loopback card index)
#
# Logs: /tmp/mA.log, /tmp/mB.log.  Drive a transfer with drive.py.
#
# Copyright (C) 2026 Rhizomatica  /  SPDX-License-Identifier: GPL-3.0-or-later
set -u
FWD=${1:-0.0}; REV=${2:-0.0}
MERCURY=${MERCURY:-./mercury}
CARD=${CARD:-2}
NB="$(dirname "$0")/noisebridge"
AF="-f S32_LE -r 48000 -c 2"

[ -x "$MERCURY" ] || { echo "mercury not found/executable at $MERCURY"; exit 1; }
[ -x "$NB" ] || { echo "noisebridge not built — run: make -C $(dirname "$0")"; exit 1; }
aplay -l 2>/dev/null | grep -qi "card $CARD: Loopback" || {
    echo "snd-aloop card $CARD not found — sudo modprobe snd-aloop"; exit 1; }

pkill -9 -x mercury 2>/dev/null
pkill -9 -f "$NB" 2>/dev/null
pkill -9 -f "arecord -D plughw:$CARD,1" 2>/dev/null
sleep 1

# Forward path:  A.play hw:C,0,0 -> capture hw:C,1,0 | fwd noise | play hw:C,0,4 -> B.capture hw:C,1,4
( arecord -D plughw:$CARD,1,0 $AF 2>/dev/null | "$NB" "$FWD" 111 | aplay -D plughw:$CARD,0,4 $AF 2>/dev/null ) &
# Reverse path:  B.play hw:C,0,5 -> capture hw:C,1,5 | rev noise | play hw:C,0,6 -> A.capture hw:C,1,6
( arecord -D plughw:$CARD,1,5 $AF 2>/dev/null | "$NB" "$REV" 222 | aplay -D plughw:$CARD,0,6 $AF 2>/dev/null ) &
sleep 1

setsid "$MERCURY" -x alsa -o plughw:$CARD,0,0 -i plughw:$CARD,1,6 -p 8300 -b 8100 -v >/tmp/mA.log 2>&1 </dev/null &
setsid "$MERCURY" -x alsa -o plughw:$CARD,0,5 -i plughw:$CARD,1,4 -p 8400 -b 8200 -v >/tmp/mB.log 2>&1 </dev/null &
sleep 4

echo "loopsim up: mercury=$(pgrep -x mercury | wc -l)/2  bridges=$(pgrep -f "arecord -D plughw:$CARD,1" | wc -l)/2  (FWD=$FWD REV=$REV)"
echo "  A: ctrl :8300 data :8301   B: ctrl :8400 data :8401   logs: /tmp/mA.log /tmp/mB.log"
echo "  drive a transfer:  python3 $(dirname "$0")/drive.py   (PAYLOAD=NNNN bytes)"
