#!/bin/bash
# MFSK decode rate against SNR3k through codec2's ch (AWGN) or the Watterson
# HF model, with the real MFSK backend at both ends (utils/mfsk_burst_file).
# AWGN goes through watterson_test with no fading path, which reads the same
# SNR3k as ch: ch never seeds its noise, so every trial would get one noise.
# Every MFSK mode keys one tone at a time at the same amplitude, so they share
# one SNR axis: the floor's burst (mode 100) reads SNR3k = -No - 11.05 through
# ch (measured; `calib` prints what ch reads for each mode).
#
#   utils/mfsk_channel_sweep.sh <awgn|good|moderate|poor|flutter> <mode> <trials> <SNR>...
#   utils/mfsk_channel_sweep.sh calib <mode>
#
# Environment: BPF="<lo> <hi>" filters what the receiver hears through a
# narrow radio filter (after the noise, as an IF filter does); FOFF=<Hz> puts
# the transmitter that far off frequency; RATE=<ratio> gives the receiver's
# sound card that clock error (1.015: 1.5 % fast); GAP="<at> <samples>" puts a
# gap in the transmitter's audio at <at> (0..1) of the burst (silence in, or
# samples out if negative).
set -u
C=$1; MODE=$2
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
TOOL="$HERE/mfsk_burst_file"; CH="$ROOT/modem/freedv/ch"; WT="$HERE/watterson_test"
[ -x "$TOOL" ] || make -C "$HERE" mfsk_burst_file >/dev/null || exit 1
[ -x "$WT" ] || make -C "$HERE" watterson_test >/dev/null || exit 1
[ -x "$CH" ] || (cd "$ROOT/modem/freedv" && gcc -O2 -std=gnu11 -I. -o ch ch.c -L. -lfreedvdata -lm) || exit 1
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
if [ "$C" = calib ]; then
  "$TOOL" tx "$MODE" 1 "$W/b.raw" "$W/f.raw"
  "$CH" "$W/b.raw" /dev/null --No -30 2>&1 | grep -i snr
  exit 0
fi
T=$3; shift 3
for s in "$@"; do
  No=$(python3 -c "print(-($s) - 11.05)"); ok=0
  for t in $(seq 1 "$T"); do
    "$TOOL" tx "$MODE" "$t" "$W/b.raw" "$W/f.raw"
    if [ -n "${GAP:-}" ]; then "$TOOL" gap $GAP "$W/f.raw" "$W/r.raw"; mv "$W/r.raw" "$W/f.raw"; fi
    if [ -n "${RATE:-}" ]; then "$TOOL" rate "$RATE" "$W/f.raw" "$W/r.raw"; mv "$W/r.raw" "$W/f.raw"; fi
    if [ "$C" = awgn ]; then F=; else F=--$C; fi
    "$WT" "$W/f.raw" "$W/n.raw" --No "$No" --freq "${FOFF:-0}" $F 2>/dev/null
    if [ -n "${BPF:-}" ]; then "$TOOL" bpf $BPF "$W/n.raw" "$W/r.raw"; mv "$W/r.raw" "$W/n.raw"; fi
    ok=$((ok + $("$TOOL" rx "$MODE" "$t" "$W/n.raw" 2>/dev/null)))
  done
  echo "$C mode=$MODE SNR3k=$s${BPF:+ bpf=${BPF// /-}}${FOFF:+ foff=$FOFF}${RATE:+ rate=$RATE}${GAP:+ gap=${GAP// /:}}: $ok/$T"
done
