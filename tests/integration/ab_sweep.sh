#!/bin/sh
# ab_sweep.sh -- run the LDPC decoder A/B grid in parallel and collect ab.csv.
#
#   sh ab_sweep.sh OUT_DIR PARALLEL "ARMS" "NO_LIST" "SEEDS" PAYLOAD_KB
#   e.g. sh ab_sweep.sh /tmp/ab 6 "trunk new emul" "-7.82 -5.82" "1 2" 0
#
# Jobs are independent (own ports, own temp dirs, own seeds), so they can run
# side by side; keep PARALLEL well under the core count because the bench is
# paced in real time and a starved station looks like a bad decoder.
set -u
OUT=$1; PAR=$2; ARMS=$3; NOS=$4; SEEDS=$5; KB=$6
mkdir -p "$OUT/jobs"
HERE=$(cd "$(dirname "$0")" && pwd)
for no in $NOS; do for seed in $SEEDS; do for arm in $ARMS; do
  echo "$arm $no $seed $KB $OUT"
done; done; done | xargs -P "$PAR" -L 1 sh "$HERE/ab_job.sh"
{ echo "arm,No_dBHz,payload_kb,seed,result,seconds,snr3k_measured,ldpc_summary"
  cat "$OUT"/jobs/*.csv | sort -t, -k2,2n -k4,4n -k1,1; } > "$OUT/ab.csv"
echo "wrote $OUT/ab.csv"
