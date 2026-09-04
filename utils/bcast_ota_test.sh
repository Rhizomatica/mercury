#!/bin/bash
# Broadcast a file between two REAL Mercury modems over the FIFO/channel bridge.
# Nothing is simulated but the propagation: real modulation, real demodulation.
set -u
R=/home/rafael2k/files/rhizomatica/hermes/mercury
MODE=${1:-1}; SIZE=${2:-5000}; NO=${3:-}
D=$(mktemp -d); PIDS=()
cleanup(){ for p in "${PIDS[@]:-}"; do kill $p 2>/dev/null; done; sleep 0.3
           for p in "${PIDS[@]:-}"; do kill -9 $p 2>/dev/null; done; [ -z "${KEEP:-}" ] && rm -rf "$D"; }
trap cleanup EXIT
[ -n "${KEEP:-}" ] && echo "logs: $D"

mkdir -p "$D/rx"
head -c "$SIZE" /dev/urandom > "$D/report.bin"

for f in a_rx a_tx b_rx b_tx; do mkfifo "$D/$f.fifo"; done
APORT=$(( 21000 + RANDOM % 2000 )); BPORT=$(( 24000 + RANDOM % 2000 ))

"$R/mercury" -x fifo -i "$D/a_rx.fifo" -o "$D/a_tx.fifo" -p $APORT -b $((APORT+100)) \
    -m "$MODE" -C "$D/none.ini" >"$D/a.log" 2>&1 & PIDS+=($!)
"$R/mercury" -x fifo -i "$D/b_rx.fifo" -o "$D/b_tx.fifo" -p $BPORT -b $((BPORT+100)) \
    -m "$MODE" -C "$D/none.ini" >"$D/b.log" 2>&1 & PIDS+=($!)

# The "air": A's TX into B's RX and back.  ch adds noise when --No is given.
CH="$R/modem/freedv/ch"
# Always stream through ch: it is built to move samples between FIFOs in real
# time.  A plain `cat` buffers in 64 kB chunks, so the receiving modem sees the
# stream in bursts and never syncs -- which looks exactly like a broken
# transfer.  "Clean" is just ch with negligible noise.
NO_EFF=${NO:--100}
"$CH" "$D/a_tx.fifo" "$D/b_rx.fifo" --No "$NO_EFF" >/dev/null 2>&1 & PIDS+=($!)
"$CH" "$D/b_tx.fifo" "$D/a_rx.fifo" --No "$NO_EFF" >/dev/null 2>&1 & PIDS+=($!)
if [ -n "$NO" ]; then
  AIR="ch --No $NO (SNR3k ~$(python3 -c "print(f'{-1*($NO)-14.82:.1f}')") dB)"
else
  AIR="ch clean (--No $NO_EFF)"
fi
sleep 3

echo "mode=$MODE  file=$SIZE B  air=$AIR"
"$R/utils/bcast_file_tool" recv "$D/rx" -m "$MODE" -p $((BPORT+100)) >"$D/recv.log" 2>&1 & RXPID=$!; PIDS+=($RXPID)
sleep 1
"$R/utils/bcast_file_tool" send "$D/report.bin" -m "$MODE" -p $((APORT+100)) >"$D/send.log" 2>&1 & PIDS+=($!)

for i in $(seq 1 ${TIMEOUT:-300}); do kill -0 $RXPID 2>/dev/null || break; sleep 1; done

if [ -f "$D/rx/report.bin" ] && cmp -s "$D/report.bin" "$D/rx/report.bin"; then
    echo "  RESULT: recovered byte-identical as \"report.bin\" in ${i}s"
    grep -o "frames [0-9]*" "$D/send.log" | tail -1 | sed 's/^/  TX /'
    exit 0
else
    echo "  RESULT: FAILED after ${i}s"
    echo "  --- recv ---"; tail -3 "$D/recv.log"
    echo "  --- send ---"; tail -3 "$D/send.log"
    echo "  --- B modem ---"; grep -iE "broadcast|invalid|error" "$D/b.log" | tail -5
    exit 1
fi
