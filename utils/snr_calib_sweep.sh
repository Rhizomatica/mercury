#!/bin/bash
# snr_calib_sweep.sh <mode index> <name> "<true SNR3k list, dB>" [port]
#
# What the modem reports as SNR (freedv_snr_calib() applied) against the true
# SNR3k of codec2's ch channel simulator (AWGN), for one mode.  Used to fit the
# per-mode lines in modem/freedv/freedv_700.c (see there).
#
#   utils/snr_calib_sweep.sh 1 DATAC3 "-2 0 2 4 6 10"
#
# 60 s of test frames (mercury -t) are generated once, put through ch at each
# noise level, and decoded by a second mercury fed in real time over a FIFO
# (reading a capture faster than real time drops audio).  Prints, per point,
# ch's SNR3k, the median reported SNR of the frames that decoded, and how many
# did.  ch's SNR3k for a given --No differs per mode (their power differs), so
# the true values do not land exactly on the list asked for.
#
# Run several modes in parallel with different ports: each mercury opens its
# TCP ports, and a second one on the same ports exits at once.
#
# Needs sox, bc, python3 and modem/freedv/ch (built with the tree).
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
M=$1; NAME=$2; SNRS=$3; PORT=${4:-7002}
D=${SNR_CALIB_DIR:-/tmp/snr_calib}/$NAME
mkdir -p "$D"; cd "$D" || exit 1
: > none.ini

if [ ! -s tx.s16 ]; then
    rm -f tx.s32; : > tx.s32
    timeout -s INT 65 "$R/mercury" -t -m "$M" -x fifo -i /dev/zero -o tx.s32 \
        -p "$PORT" -b $((PORT + 5)) -C "$D/none.ini" > txgen.log 2>&1
    sox -t raw -e signed -b 32 -r 8000 -c 1 tx.s32 -t raw -e signed -b 16 tx.s16
fi

pace() {    # copy a file into a FIFO at real time (32000 B/s), then 3 s of silence
    python3 - "$1" "$2" <<'EOF'
import sys, time
src, dst = sys.argv[1], sys.argv[2]
rate = 32000; chunk = rate // 50
data = open(src, 'rb').read() + bytes(rate * 3)
t0 = time.monotonic()
try:
    with open(dst, 'wb', buffering=0) as f:
        for i in range(0, len(data), chunk):
            f.write(data[i:i + chunk])
            ahead = (i + chunk) / rate - (time.monotonic() - t0)
            if ahead > 0:
                time.sleep(ahead)
except BrokenPipeError:
    pass
EOF
}

for S in $SNRS; do
    No=$(echo "-($S) - 14.82" | bc -l)
    "$R/modem/freedv/ch" tx.s16 rx.s16 --No "$No" > ch.log 2>&1
    truth=$(grep -o "SNR3k(dB): *[-0-9.]*" ch.log | grep -o "[-0-9.]*$")
    sox -t raw -e signed -b 16 -r 8000 -c 1 rx.s16 -t raw -e signed -b 32 rx.s32
    rm -f rx.fifo; mkfifo rx.fifo
    timeout 150 "$R/mercury" -m "$M" -x fifo -i rx.fifo -o /dev/null -v \
        -p "$PORT" -b $((PORT + 5)) -C "$D/none.ini" > rx.log 2>&1 &
    mp=$!
    pace rx.s32 rx.fifo
    sleep 2; kill -INT $mp 2>/dev/null; wait $mp 2>/dev/null
    n=$(grep -c "Decoded frame" rx.log)
    med=$(grep "Decoded frame" rx.log | grep -o "snr=[-0-9.]*" | cut -d= -f2 | sort -n |
          awk '{a[NR]=$1} END{if (NR) print a[int((NR+1)/2)]; else print "-"}')
    echo "$NAME true=${truth:-?} reported_median=$med decoded=$n"
done
