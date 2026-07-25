#!/bin/bash
# fastack_lowsnr_ab.sh — LOW-SNR fringe robustness A/B for the fast windowed ACK.
# 5 kB payload (a uucp-sized picture), AWGN sweep from ~+5 down through <0 dB,
# fast-ACK OFF vs ON.  At the fringe the ladder sits at the MFSK/DATAC4 floor
# where the fast ACK does NOT fire (bursts are K=1 / holey), so the test proves
# the SACRED property: ON must still COMPLETE and must NOT regress vs OFF at low
# SNR.  Records the TRUE snr3k (from channel_sim NP_STATS act_rms), not the
# modem's capped self-estimate.
#
#   MERCURY_BIN=/path/to/mercury-arq-windowed utils/fastack_lowsnr_ab.sh
# Sweep points (sigma -> nominal snr3k = 9 + 20log10(8198/sigma)):
set +u
SW="${SKYWAVE_DIR:-/home/rafael2k/files/rhizomatica/hermes/skywave}"
BIN="${MERCURY_BIN:?set MERCURY_BIN}"
BYTES="${BYTES:-5632}"; DL="${DL:-600}"
SIGMAS=(${SIGMAS:-13000 16400 20600 26000 33000})   # ~ +5 +3 +1 -1 -3 dB
CSV=/tmp/fastack_lowsnr_ab.csv
echo "sigma,snr3k_true,fast_ack,got,total,intact,goodput_Bps,status" > "$CSV"

nuke(){ pkill -9 -f -- '-x sock' 2>/dev/null; pkill -9 -f mercury_sock 2>/dev/null;
        pkill -9 -f channel_sim 2>/dev/null; sleep 3; }

snr_from_np(){  # $1 = NP_STATS base path, $2 = sigma -> true snr3k (dir A->B = .11)
  python3 - "$1" "$2" <<'PY' 2>/dev/null
import sys,json,math
base,sig=sys.argv[1],float(sys.argv[2])
best=None
for suf in (".11",".22"):
    try:
        d=json.load(open(base+suf)); a=float(d.get("act_rms",0))
        if a>0:
            v=round(9.0+20*math.log10(a*1.0)-20*math.log10(sig),1) if sig>0 else 99.0
            # snr3k_measured convention: 9 + 20log10(act_rms) - 20log10(sigma)  (ref-scaled)
            best=v if best is None else best
    except Exception: pass
print(best if best is not None else "?")
PY
}

run_one(){  # $1=sigma $2=fast_ack
  local sig="$1" fa="$2"
  local D="/tmp/low_${sig}_fa${fa}"; rm -rf "$D"; mkdir -p "$D"
  local NP="/tmp/np_${sig}_fa${fa}"
  local PROF="/tmp/lowsnr_${sig}.toml"
  printf 'name = "lowsnr_%s"\ndescription = "AWGN sigma=%s"\nsigma = %s\n' "$sig" "$sig" "$sig" > "$PROF"
  nuke
  local R
  R=$(MERCURY_BIN="$BIN" MERCURY_FAST_ACK="$fa" PYTHONPATH="$SW/src" \
      SIM_PROFILE="$PROF" SIM_SOCK_DIR="$D" SIM_PTT=1 SIM_HALF_DUPLEX=1 NP_STATS="$NP" \
      timeout "$DL" python3 -u -m skywave.adapters.mercury_sock "$BYTES" "$DL" 2>&1 \
      | grep -aoE 'RESULT:.*')
  local got tot intact gp snr status
  got=$(sed -nE 's#.*RESULT: ([0-9]+)/[0-9]+ B .*#\1#p' <<<"$R")
  tot=$(sed -nE 's#.*RESULT: [0-9]+/([0-9]+) B .*#\1#p' <<<"$R")
  intact=$(sed -nE 's#.*intact=([A-Za-z]+).*#\1#p' <<<"$R")
  gp=$(sed -nE 's#.*goodput=([0-9.]+) B/s.*#\1#p' <<<"$R")
  snr=$(snr_from_np "$NP" "$sig")
  [ -n "$R" ] && status=ok || status=timeout
  echo "[sigma=$sig snr3k=$snr fa=$fa] ${R:-<timeout>}"
  echo "${sig},${snr},${fa},${got:-0},${tot:-$BYTES},${intact:-false},${gp:-0},${status}" >> "$CSV"
}

echo "===== FAST-ACK LOW-SNR A/B — ${BYTES}B, sigma: ${SIGMAS[*]} ====="
for sig in "${SIGMAS[@]}"; do
  run_one "$sig" 0     # OFF
  run_one "$sig" 1     # ON
done
nuke
echo "===== DONE — $CSV ====="
column -t -s, "$CSV"
