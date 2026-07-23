#!/bin/bash
# burst_rx_sweep.sh — multi-frame-burst RX gate for the windowed ARQ leap.
#
# Validates, at the raw codec2 layer (no mercury), the three facts Phase 1 of
# the windowed ARQ rests on:
#   1. Every OFDM ladder mode decodes K-frame bursts behind ONE preamble when
#      the RX knows K (freedv_set_frames_per_burst == TX framesperburst).
#   2. Shared-preamble bursts decode as well as single-frame bursts under noise
#      (no robustness penalty from windowing itself).
#   3. The partial-burst pathology: if the RX expects more frames than a burst
#      carries (J < K_rx), the OFDM burst state machine has NO exit from
#      `synced` except packet count (no UW check while synced), so it consumes
#      (K_rx - J) packet-durations of FOLLOWING audio as garbage — eating the
#      next keydown's preamble.  This is why the windowed ARQ makes bursts
#      SELF-DESCRIBING (frames-remaining in every DATA header) instead of
#      relying on a negotiated/mirrored K.
#
# Run from the repo root:  bash utils/burst_rx_sweep.sh
# Exit code 0 = all gates pass.
set -u
cd "$(dirname "$0")/../modem/freedv" || exit 1
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fail=0

rx_frames() { # mode fpb file -> decoded frame count
  ./freedv_data_raw_rx --testframes --framesperburst "$2" "$1" "$3" /dev/null 2>&1 |
    grep -oE 'Frms\.:[[:space:]]+[0-9]+' | grep -oE '[0-9]+'
}

echo "== gate 1: K=5 bursts, clean, all ladder modes (expect 10/10) =="
for M in DATAC15 DATAC4 DATAC3 DATAC1 DATAC17 QAM16C2 DATAC16; do
  ./freedv_data_raw_tx --testframes 10 --bursts 2 --framesperburst 5 --delay 500 \
      "$M" /dev/zero "$W/t.raw" 2>/dev/null
  n=$(rx_frames "$M" 5 "$W/t.raw")
  printf '  %-9s %s/10 %s\n' "$M" "${n:-0}" "$( [ "${n:-0}" -eq 10 ] && echo ok || { echo FAIL; fail=1; } )"
done

echo "== gate 2: DATAC3 noise parity K=1 vs K=4 @ ch --No -17 (expect 20/20 both) =="
./freedv_data_raw_tx --testframes 20 --bursts 20 --framesperburst 1 --delay 500 DATAC3 /dev/zero "$W/n1.raw" 2>/dev/null
./freedv_data_raw_tx --testframes 20 --bursts 5  --framesperburst 4 --delay 500 DATAC3 /dev/zero "$W/n4.raw" 2>/dev/null
./ch "$W/n1.raw" "$W/n1c.raw" --No -17 2>/dev/null
./ch "$W/n4.raw" "$W/n4c.raw" --No -17 2>/dev/null
n1=$(rx_frames DATAC3 1 "$W/n1c.raw"); n4=$(rx_frames DATAC3 4 "$W/n4c.raw")
printf '  K=1 %s/20, K=4 %s/20 %s\n' "${n1:-0}" "${n4:-0}" \
  "$( [ "${n1:-0}" -eq 20 ] && [ "${n4:-0}" -eq 20 ] && echo ok || { echo FAIL; fail=1; } )"

echo "== gate 3: partial-burst pathology (documented failure mode, expect 2/6) =="
./freedv_data_raw_tx --testframes 2 --bursts 1 --framesperburst 2 --delay 500 DATAC3 /dev/zero "$W/j2.raw" 2>/dev/null
./freedv_data_raw_tx --testframes 4 --bursts 1 --framesperburst 4 --delay 500 DATAC3 /dev/zero "$W/j4.raw" 2>/dev/null
cat "$W/j2.raw" "$W/j4.raw" > "$W/p.raw"
np=$(rx_frames DATAC3 4 "$W/p.raw")
printf '  [2-burst][4-burst] rx fpb=4 -> %s/6 (partial burst eats the next keydown: %s)\n' \
  "${np:-0}" "$( [ "${np:-0}" -le 2 ] && echo 'reproduced, self-describing bursts required' || echo 'NOT reproduced — recheck design assumption' )"

echo
[ $fail -eq 0 ] && echo "burst_rx_sweep: ALL GATES PASS" || echo "burst_rx_sweep: FAILURES"
exit $fail
