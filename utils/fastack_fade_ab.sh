#!/bin/bash
# fastack_fade_ab.sh — end-to-end fast-windowed-ACK A/B over Watterson-faded
# channels, via the skywave mercury_sock adapter (virtual clock).  Answers the
# two questions that gate MERCURY_FAST_ACK before OTA:
#   SPEED      — does fast-ACK ON improve goodput on good/mid channels?
#   ROBUSTNESS — does ON ever regress vs OFF under fading? (it must not)
#                incl. the reverse/ACK-path-buried 'poor-weak-ack' worst case.
#
# Same binary both arms; only MERCURY_FAST_ACK differs.  Full rig cleanup
# between runs (the -x sock rig degrades across same-session runs otherwise).
#
#   MERCURY_BIN=/path/to/mercury-arq-windowed \
#     utils/fastack_fade_ab.sh [profiles...] [--bytes N] [--reps R] [--dl S]
#
# Profiles are skywave/profiles/*.toml names (default: clean mid11 poor
# poor-weak-ack).  Results appended to /tmp/fastack_fade_ab.csv.
set +e
SW="${SKYWAVE_DIR:-/home/rafael2k/files/rhizomatica/hermes/skywave}"
BIN="${MERCURY_BIN:?set MERCURY_BIN to the arq-windowed mercury binary}"
BYTES=16384; REPS=1; DL=500
PROFILES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --bytes) BYTES="$2"; shift 2;;
    --reps)  REPS="$2";  shift 2;;
    --dl)    DL="$2";    shift 2;;
    *)       PROFILES+=("$1"); shift;;
  esac
done
[ "${#PROFILES[@]}" -eq 0 ] && PROFILES=(clean mid11 poor poor-weak-ack)

CSV=/tmp/fastack_fade_ab.csv
[ -f "$CSV" ] || echo "profile,fast_ack,rep,bytes,got,intact,goodput_Bps,wall_s,status" > "$CSV"

nuke(){ pkill -9 -f -- '-x sock' 2>/dev/null; pkill -9 -f mercury_sock 2>/dev/null;
        pkill -9 -f channel_sim 2>/dev/null; sleep 3; }

run_one(){  # $1=profile $2=fast_ack(0/1) $3=rep
  local prof="$1" fa="$2" rep="$3"
  local D="/tmp/fade_${prof}_fa${fa}_r${rep}"; rm -rf "$D"; mkdir -p "$D"
  nuke
  local R
  R=$(MERCURY_BIN="$BIN" MERCURY_FAST_ACK="$fa" PYTHONPATH="$SW/src" \
      SIM_PROFILE="$SW/profiles/${prof}.toml" SIM_SOCK_DIR="$D" SIM_PTT=1 SIM_HALF_DUPLEX=1 \
      timeout "$DL" python3 -u -m skywave.adapters.mercury_sock "$BYTES" "$DL" 2>&1 \
      | grep -aoE 'RESULT:.*')
  echo "[${prof} fa=${fa} r${rep}] ${R:-<no RESULT / timeout>}"
  # parse RESULT: NNN/NNN B in Xs intact=T goodput=G B/s ... wall=Ws
  local got tot intact gp wall status
  got=$(sed -nE 's#.*RESULT: ([0-9]+)/[0-9]+ B .*#\1#p' <<<"$R")
  tot=$(sed -nE 's#.*RESULT: [0-9]+/([0-9]+) B .*#\1#p' <<<"$R")
  intact=$(sed -nE 's#.*intact=([A-Za-z]+).*#\1#p' <<<"$R")
  gp=$(sed -nE 's#.*goodput=([0-9.]+) B/s.*#\1#p' <<<"$R")
  wall=$(sed -nE 's#.*wall=([0-9.]+)s.*#\1#p' <<<"$R")
  [ -n "$R" ] && status=ok || status=timeout
  echo "${prof},${fa},${rep},${BYTES},${got:-0},${intact:-false},${gp:-0},${wall:-0},${status}" >> "$CSV"
}

echo "===== FAST-ACK FADE A/B — profiles: ${PROFILES[*]} | ${BYTES}B x${REPS} ====="
for prof in "${PROFILES[@]}"; do
  for rep in $(seq 1 "$REPS"); do
    run_one "$prof" 0 "$rep"     # OFF (control)
    run_one "$prof" 1 "$rep"     # ON  (fast ACK)
  done
done
nuke
echo "===== DONE — results in $CSV ====="
column -t -s, "$CSV"
