#!/bin/sh
# ab_job.sh -- one end-to-end A/B point for the LDPC decoder change.
#
#   sh ab_job.sh ARM NO_DBHZ SEED PAYLOAD_KB OUT_DIR
#
# ARM   trunk  pristine trunk binary on both stations (MERCURY_TRUNK_BIN)
#       new    this tree's binary, defaults (per-code decoder policy,
#              100 iterations under a deadline, calibrated LLR scale)
#       emul   this tree's binary forced to the deployed decoder path
#              (LDPC_ALG=legacy LDPC_MAX_ITER=10 LDPC_LLR_CALIBRATED=0):
#              isolates the decoder from any other difference to trunk
# NO_DBHZ     AWGN noise density for the Watterson channel (SNR3k ~ -No-14.82;
#             the MEASURED SNR3k from the sock bench is what gets recorded)
# PAYLOAD_KB  0 = the harness default (102 bytes, 3 DATAC15 frames)
#
# Runs TestMercuryARQTransfer over the deterministic -x sock bench, paced
# (MERCURY_TEST_SOCK_FAST unset: the DSP threads see a real timeline) and
# writes one CSV line to OUT_DIR/jobs/<arm>_no<No>_s<seed>_kb<kb>.csv:
#   arm,No_dBHz,payload_kb,seed,result,seconds,snr3k_measured,ldpc_summary
set -u
ARM=$1; NO=$2; SEED=$3; KB=$4; OUT=$5
TAG="${ARM}_no${NO}_s${SEED}_kb${KB}"
LD="$OUT/logs/$TAG"; mkdir -p "$LD" "$OUT/jobs"
cd "$(dirname "$0")" || exit 1
TRUNK=${MERCURY_TRUNK_BIN:-/Users/josephfreivald/workspace/mercury-trunk/mercury}
case "$ARM" in
  trunk) EXTRA="MERCURY_TEST_BIN_A=$TRUNK MERCURY_TEST_BIN_B=$TRUNK" ;;
  new)   EXTRA="LDPC_ALG=auto" ;;
  emul)  EXTRA="LDPC_ALG=legacy LDPC_MAX_ITER=10 LDPC_LLR_CALIBRATED=0" ;;
  custom*) EXTRA="$AB_EXTRA" ;;      # ad-hoc arm: environment from AB_EXTRA
  *) echo "bad arm $ARM" >&2; exit 2 ;;
esac
PAY=""; [ "$KB" != 0 ] && PAY="MERCURY_TEST_PAYLOAD_KB=$KB"
# Parallel jobs race on the harness's free-port probe; a station that lost
# the race dies in its first second with "TCP init failed".  That is the
# bench, not the link, so the point is simply run again.
try=0
while :; do
  t0=$(date +%s)
  if env $EXTRA $PAY MERCURY_TEST_TRANSPORT=sock MERCURY_CH_ENGINE=watterson \
         MERCURY_WATTERSON_SEED="$SEED" MERCURY_CH_NO="$NO" MERCURY_TEST_LOGDIR="$LD" \
         go test -run 'TestMercuryARQTransfer$' -count=1 -timeout 25m > "$LD/go.log" 2>&1; then
    res=pass
  else
    res=fail
  fi
  t1=$(date +%s)
  try=$((try+1))
  if [ "$res" = fail ] && [ $try -lt 4 ] && grep -q 'TCP init failed\|Could not open TCP port' "$LD/go.log"; then
    echo "$TAG: port collision, retry $try" >&2; continue
  fi
  break
done
snr=$(grep -o 'SOCKSIM measured SNR3k: .*' "$LD/go.log" | head -1 | sed 's/SOCKSIM measured SNR3k: //' | tr ',' ';')
summ=$(grep -h 'decodes, mean' "$LD"/B.stderr.log "$LD"/A.stderr.log 2>/dev/null \
       | sed 's/^.*\[ldpc\] //' | tr '\n' '|' | tr ',' ' ')
echo "$ARM,$NO,$KB,$SEED,$res,$((t1-t0)),\"$snr\",\"$summ\"" > "$OUT/jobs/$TAG.csv"
echo "$TAG -> $res in $((t1-t0)) s  [$snr]"
