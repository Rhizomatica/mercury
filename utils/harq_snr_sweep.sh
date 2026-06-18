#!/bin/bash
# HARQ low-SNR characterisation sweep over the -15..0 dB field (the HF fringe).
#
# Emits B bit-identical CRC-protected bursts (each its own preamble = a Mercury
# retransmission), pipes them through the FIXED Watterson HF channel (ITU-R
# good/moderate/poor) or codec2 ch.c (AWGN reference), and decodes each faded
# file twice: single-shot and with HARQ Chase soft-combining (--harq). Reports
# the channel's MEASURED mean SNR3k and CRC-valid delivery for each.
#
# The Watterson tool is calibrated to ch.c: SNR3k = -No - 14.82 (a static
# 1-path Watterson run reproduces ch's AWGN delivery exactly).
#
#   usage: utils/harq_snr_sweep.sh [MODE] [CHANNEL] [BURSTS]
#     MODE    : DATAC4|DATAC3|DATAC1|DATAC15|... (default DATAC3)
#     CHANNEL : awgn|good|moderate|poor|flutter  (default poor)
#     BURSTS  : identical retransmissions per SNR point (default 120)
set -u
MODE=${1:-DATAC3}; CH=${2:-poor}; B=${3:-120}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
FV="$ROOT/modem/freedv"; WT="$HERE/watterson_test"
TX="$FV/freedv_data_raw_tx"; RX="$FV/freedv_data_raw_rx"; CHB="$FV/ch"

# build what's missing
[ -x "$WT" ] || make -C "$HERE" watterson_test >/dev/null || exit 1
for t in freedv_data_raw_tx freedv_data_raw_rx ch; do
  [ -x "$FV/$t" ] || (cd "$FV" && gcc -Wall -O2 -std=gnu11 -I. -o "$t" "$t.c" -L. -lfreedvdata -lm) >/dev/null 2>&1
done

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
"$TX" "$MODE" --testframes "$B" --bursts "$B" /dev/zero "$D/tx.raw" 2>/dev/null

deliv() { "$RX" ${1:-} --testframes "$MODE" "$D/f.raw" /dev/null 2>&1 \
          | awk '/Coded FER/{d=$5-$7; printf "%d/%d (%d%%)", d,$5,(($5)?100*d/$5:0)}'; }

echo "# HARQ low-SNR sweep  mode=$MODE  channel=$CH  bursts=$B"
printf "%-12s %-16s %-16s\n" "meanSNR3k" "single-shot" "HARQ"
# No chosen so SNR3k = -No-14.82 spans ~0 down to ~-15 dB
for No in -14.82 -13.82 -12.82 -11.82 -10.82 -9.82 -8.82 -7.82 -6.82 -5.82 -4.82 -3.82 -2.82 -1.82 -0.82 0.18; do
  if [ "$CH" = awgn ]; then
    "$CHB" "$D/tx.raw" "$D/f.raw" --No "$No" 2>/dev/null
    snr=$(python3 -c "print(f'{-($No)-14.82:.2f}')")
  else
    "$WT" "$D/tx.raw" "$D/f.raw" --No "$No" --"$CH" 2>/dev/null
    snr=$("$WT" "$D/tx.raw" /dev/null --No "$No" --"$CH" 2>&1 >/dev/null \
          | grep -oE 'SNR3k\(dB\):[ ]*[-0-9.]+' | grep -oE '[-0-9.]+$')
  fi
  printf "%-12s %-16s %-16s\n" "${snr:-?}" "$(deliv)" "$(deliv --harq)"
done
