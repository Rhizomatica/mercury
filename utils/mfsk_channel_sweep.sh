#!/bin/bash
# MFSK decode rate against SNR through codec2's ch (AWGN) or the Watterson
# HF model, on the floor's SNR axis: SNR3k as the floor's burst (mode 100)
# would have it at the same noise, so modes at the same peak level compare
# at the same transmitter.  (Mode 101 at its own level reads 3.1 dB lower.)
#
#   utils/mfsk_channel_sweep.sh <awgn|good|moderate|poor> <mode> <trials> <SNR>...
set -u
C=$1; MODE=$2; T=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
TOOL="$HERE/mfsk_burst_file"; CH="$ROOT/modem/freedv/ch"; WT="$HERE/watterson_test"
[ -x "$TOOL" ] || make -C "$HERE" mfsk_burst_file >/dev/null || exit 1
[ -x "$WT" ] || make -C "$HERE" watterson_test >/dev/null || exit 1
[ -x "$CH" ] || (cd "$ROOT/modem/freedv" && gcc -O2 -std=gnu11 -I. -o ch ch.c -L. -lfreedvdata -lm) || exit 1
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# The floor's burst reads SNR3k = -No - 11.05 through ch (measured).
for s in "$@"; do
  No=$(python3 -c "print(-($s) - 11.05)"); ok=0
  for t in $(seq 1 "$T"); do
    "$TOOL" tx "$MODE" "$t" "$W/b.raw" "$W/f.raw"
    if [ "$C" = awgn ]; then "$CH" "$W/f.raw" "$W/n.raw" --No "$No" 2>/dev/null
    else "$WT" "$W/f.raw" "$W/n.raw" --No "$No" --"$C" 2>/dev/null; fi
    ok=$((ok + $("$TOOL" rx "$MODE" "$t" "$W/n.raw")))
  done
  echo "$C mode=$MODE SNR3k(floor)=$s: $ok/$T"
done
