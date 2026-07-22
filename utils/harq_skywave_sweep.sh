#!/bin/bash
# HARQ low-SNR sweep through the skywave (OpenARQ) HF channel simulator, with a
# CALIBRATED SNR3k axis.  Companion to harq_snr_sweep.sh (which uses codec2 ch /
# the Watterson tool); this one drives skywave's richer fade + rig model.
#
# Emits B bit-identical CRC-protected bursts (each its own preamble = a Mercury
# retransmission), pipes them through skywave's hfchan, and decodes each faded
# file twice: single-shot and with HARQ Chase soft-combining (--harq).
#
# NOISE-AXIS CALIBRATION (important): hfchan is codec2-`ch`-compatible for the
# FADE, but its --No noise axis is NOT ch's.  skywave's cross-calibration
# decoupled noise (it adds noise via an external injector), so --No parity was
# never intended.  Measured: hfchan noise power tracks --No at slope 1 dB/dB and
# its signal power equals ch's, so the difference is a pure constant offset.
# Pinned against ch (whose SNR3k = -No - 14.93) via the DATAC17 AWGN cliff:
# hfchan runs ~3.15 dB hotter, so
#         true SNR3k(dB) = -(hfchan_No) - 18.1        (= -No - 14.93 - 3.15)
# The offset is mode-independent (property of the tools, not the modem).  This
# script therefore sweeps by TARGET SNR3k and converts to hfchan --No.
#
# GATE A/B (PR #127, HARQ parity-gate goodput neutrality): build a second
# freedv_data_raw_rx from the branch under test, then decode the SAME f.raw with
# both binaries under --harq and diff the delivered counts (they must match --
# the gate must reject no genuine combined delivery).
#
#   usage: utils/harq_skywave_sweep.sh [MODE] [CHANNEL] [BURSTS]
#     MODE    : DATAC15|DATAC17|DATAC4|DATAC3|... (default DATAC17)
#     CHANNEL : awgn|mpg|mpp|mpd  (hfchan fade; mpp = CCIR poor 2ms/1Hz; default mpp)
#     BURSTS  : identical retransmissions per SNR point (default 40)
#   env: SKYWAVE_DIR  (default: <repo>/../skywave)
set -u
MODE=${1:-DATAC17}; CH=${2:-mpp}; B=${3:-40}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
FV="$ROOT/modem/freedv"
SW=${SKYWAVE_DIR:-$ROOT/../skywave}
TX="$FV/freedv_data_raw_tx"; RX="$FV/freedv_data_raw_rx"
CAL=18.1   # true SNR3k = -No - CAL  (see header)

if [ ! -d "$SW/src/skywave" ]; then
  echo "skywave not found at '$SW' — set SKYWAVE_DIR to the skywave checkout" >&2
  exit 1
fi
# build the freedv CLI tools if missing (mirrors harq_snr_sweep.sh)
make -C "$FV" >/dev/null 2>&1 || true
for t in freedv_data_raw_tx freedv_data_raw_rx; do
  [ -x "$FV/$t" ] || (cd "$FV" && gcc -Wall -O2 -std=gnu11 -I. -o "$t" "$t.c" \
     -L. -lfreedvdata -lm) >/dev/null 2>&1 || { echo "build $t failed" >&2; exit 1; }
done

fade=""; [ "$CH" != awgn ] && fade="--$CH"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
"$TX" "$MODE" --testframes "$B" --bursts "$B" /dev/zero "$D/tx.raw" 2>/dev/null

deliv() { "$RX" ${1:-} --testframes "$MODE" "$D/f.raw" /dev/null 2>&1 \
          | awk '/Coded FER/{d=$5-$7; printf "%d/%d (%d%%)", d,$5,(($5)?100*d/$5:0)}'; }

echo "# HARQ via skywave/hfchan  mode=$MODE  channel=$CH  bursts=$B  seed=1"
echo "# SNR3k calibrated to codec2 ch: hfchan --No = -(SNR3k) - $CAL"
printf "%-10s %-18s %-18s\n" "SNR3k" "single-shot" "HARQ"
for SNR in 6 5 4 3 2 1 0 -1 -2 -3 -4 -5; do
  No=$(python3 -c "print(f'{-($SNR)-$CAL:.2f}')")
  PYTHONPATH="$SW/src" python3 -m skywave.hfchan --No "$No" $fade --seed 1 \
     "$D/tx.raw" "$D/f.raw" 2>/dev/null
  printf "%-10s %-18s %-18s\n" "$SNR" "$(deliv)" "$(deliv --harq)"
done
