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
# Usage: run_loopsim.sh [FWD_NOISE] [REV_NOISE]   start (restarts a running one)
#        run_loopsim.sh stop                       stop what this script started
#   FWD_NOISE / REV_NOISE : noisebridge stddev fraction (default 0.0 = clean).
#                           ~1.0 ≈ -4 dB SNR; ~2.0 stalls a marginal ACK path.
# Env overrides: MERCURY=./mercury  CARD=N (loopback card; auto-detected)
#                LOOPSIM_PIDFILE=/tmp/loopsim.pids
#
# Logs: /tmp/mA.log, /tmp/mB.log.  Drive a transfer with drive.py.
#
# Three things this script used to get wrong, each of which cost a debugging
# session:
#   - It killed with `pkill -9 -x mercury`: EVERY mercury on the host, a real
#     station or another test included.  It now kills only the process groups
#     it started, recorded in LOOPSIM_PIDFILE.
#   - The background audio pipelines inherited this script's stdout, so anything
#     capturing its output -- $(run_loopsim.sh), `| tail` -- hung until the
#     pipelines died.  They are now detached with their own stdio.
#   - The loopback card defaulted to 2; it is whatever index snd-aloop got, so
#     it is now read from `aplay -l`.
#
# Copyright (C) 2026 Rhizomatica  /  SPDX-License-Identifier: GPL-3.0-or-later
set -u
PIDFILE=${LOOPSIM_PIDFILE:-/tmp/loopsim.pids}

stop_loopsim() {
    [ -f "$PIDFILE" ] || return 0
    while read -r pid; do
        [ -n "$pid" ] || continue
        # Each entry leads its own process group (started via setsid), so kill
        # the whole group: a pipeline is arecord | noisebridge | aplay.
        kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null
    done < "$PIDFILE"
    rm -f "$PIDFILE"
    sleep 1
}

if [ "${1:-}" = "stop" ]; then
    stop_loopsim
    echo "loopsim stopped"
    exit 0
fi

FWD=${1:-0.0}; REV=${2:-0.0}
MERCURY=${MERCURY:-./mercury}
NB="$(cd "$(dirname "$0")" && pwd)/noisebridge"
AF="-f S32_LE -r 48000 -c 2"
CARD=${CARD:-$(aplay -l 2>/dev/null | awk '/Loopback/ { sub(/:$/, "", $2); print $2; exit }')}

[ -x "$MERCURY" ] || { echo "mercury not found/executable at $MERCURY"; exit 1; }
[ -x "$NB" ] || { echo "noisebridge not built — run: make -C $(dirname "$0")"; exit 1; }
[ -n "$CARD" ] && aplay -l 2>/dev/null | grep -qi "card $CARD: Loopback" || {
    echo "snd-aloop loopback card not found — sudo modprobe snd-aloop"; exit 1; }

stop_loopsim   # a previous run of THIS script, and nothing else

# start_group CMD... : run detached in its own session with its own stdio, and
# record its pid (== its process-group id) for stop_loopsim.
start_group() {
    setsid "$@" </dev/null >/dev/null 2>&1 &
    echo $! >> "$PIDFILE"
}

export AF NB CARD FWD REV
# Forward path:  A.play hw:C,0,0 -> capture hw:C,1,0 | fwd noise | play hw:C,0,4 -> B.capture hw:C,1,4
start_group bash -c 'arecord -D plughw:$CARD,1,0 $AF | "$NB" "$FWD" 111 | aplay -D plughw:$CARD,0,4 $AF'
# Reverse path:  B.play hw:C,0,5 -> capture hw:C,1,5 | rev noise | play hw:C,0,6 -> A.capture hw:C,1,6
start_group bash -c 'arecord -D plughw:$CARD,1,5 $AF | "$NB" "$REV" 222 | aplay -D plughw:$CARD,0,6 $AF'
sleep 1

setsid "$MERCURY" -x alsa -o plughw:$CARD,0,0 -i plughw:$CARD,1,6 -p 8300 -b 8100 -v >/tmp/mA.log 2>&1 </dev/null &
echo $! >> "$PIDFILE"
setsid "$MERCURY" -x alsa -o plughw:$CARD,0,5 -i plughw:$CARD,1,4 -p 8400 -b 8200 -v >/tmp/mB.log 2>&1 </dev/null &
echo $! >> "$PIDFILE"
sleep 4

alive=0
while read -r pid; do kill -0 "$pid" 2>/dev/null && alive=$((alive+1)); done < "$PIDFILE"
echo "loopsim up on card $CARD: $alive/4 process groups alive  (FWD=$FWD REV=$REV)"
echo "  A: ctrl :8300 data :8301   B: ctrl :8400 data :8401   logs: /tmp/mA.log /tmp/mB.log"
echo "  drive a transfer:  python3 $(dirname "$0")/drive.py   (PAYLOAD=NNNN bytes)"
echo "  stop:              $0 stop"
