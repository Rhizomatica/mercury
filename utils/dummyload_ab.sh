#!/bin/bash
# Windowed-ARQ A/B on the dummy-load bench (estacao2 <-> estacao3).
#
# Deploys one build at a time to BOTH stations, runs a timed uucp transfer, and
# appends a CSV row per run.  Builds are INTERLEAVED (trunk, rethink, windowed,
# trunk, ...) because the bench link drifts over tens of minutes: running one
# build to completion and then the next bakes that drift into the comparison.
#
# See docs/DUMMY-LOAD-TEST-PLAN.md for the protocol and the rules this script
# encodes (never restart sbitx; one mercury per station; drive from a script
# file so pkill cannot match its own argv; clear stale uucp jobs first).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

SENDER_IP=${SENDER_IP:-192.168.10.234}      # estacao2 / PU2UIT-2 (Pi 5)
RECEIVER_IP=${RECEIVER_IP:-192.168.10.106}  # estacao3 / PU2UIT-3 (Pi 4)
RECEIVER_SYS=${RECEIVER_SYS:-PU2UIT-3}
SSH_PASS=${SSH_PASS:-hermes}

BUILDS="trunk,rethink,windowed"
REPS=3
PAYLOAD=/root/test_file
OUT=results-bench.csv
SETTLE=14          # seconds for uucpd to re-attach after a mercury restart

declare -A BRANCH=(
  [trunk]=mercuryv2
  [rethink]=mfsk-arq-integration
  [windowed]=arq-windowed
)

usage() {
    cat <<EOF
usage: $0 [--builds trunk,rethink,windowed] [--reps N] [--payload PATH] [--out FILE]

  --builds   comma-separated subset of: ${!BRANCH[@]}
  --reps     repetitions per build (default $REPS; 1 proves nothing)
  --payload  file on the sender to transfer (default $PAYLOAD)
  --out      CSV output (default $OUT)

env: SENDER_IP RECEIVER_IP RECEIVER_SYS SSH_PASS
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --builds)  BUILDS=$2; shift 2 ;;
        --reps)    REPS=$2; shift 2 ;;
        --payload) PAYLOAD=$2; shift 2 ;;
        --out)     OUT=$2; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1"; usage ;;
    esac
done

command -v sshpass >/dev/null || { echo "need sshpass"; exit 1; }

# Serial SSH with password auth: parallel connections trip sshd MaxStartups and
# the run hangs half-way through with no useful error.
sh_() {
    local ip=$1; shift
    sshpass -p "$SSH_PASS" ssh -o PreferredAuthentications=password \
        -o NumberOfPasswordPrompts=1 -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "pi@$ip" "$@"
}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { printf '[%s] ERROR: %s\n' "$(date +%H:%M:%S)" "$*" >&2; exit 1; }

# ---- pre-flight -------------------------------------------------------------
# Each of these has burned a real run at some point; failing here is cheap.
preflight() {
    local ip name
    for ip in "$SENDER_IP" "$RECEIVER_IP"; do
        name=$(sh_ "$ip" hostname 2>/dev/null) || die "cannot reach $ip"

        sh_ "$ip" "pgrep -c sbitx" >/dev/null 2>&1 \
            || log "WARNING: sbitx not running on $ip ($name) — is the rig up?"

        local free
        free=$(sh_ "$ip" "df -P /root | awk 'NR==2{print \$4}'")
        [ "${free:-0}" -gt 102400 ] || die "$ip: less than 100 MB free on /root"

        log "$ip ($name): reachable, disk ok"
    done

    sh_ "$SENDER_IP" "test -f $PAYLOAD" || die "payload $PAYLOAD missing on sender"
}

# ---- deploy -----------------------------------------------------------------
# Build from source on each station and restart mercury WITHOUT touching sbitx:
# restarting sbitx drifts the gain/AGC and silently changes the link under the
# measurement.
deploy() {
    local branch=$1 ip
    for ip in "$SENDER_IP" "$RECEIVER_IP"; do
        log "deploying $branch to $ip"
        # Heredoc into a remote script file: mercury must not appear in the argv
        # of the shell that runs pkill, or pkill matches itself.
        sh_ "$ip" "sudo tee /root/deploy.sh >/dev/null" <<EOF
#!/bin/bash
set -e
cd /root/mercury
git fetch origin $branch
git checkout -B $branch FETCH_HEAD
make -j\$(nproc) >/tmp/build.log 2>&1
pkill -TERM mercury || true
sleep 5
pkill -9 mercury || true
sleep 1
install -m755 /root/mercury/mercury /usr/bin/mercury
cd /root/mercury
setsid -f /usr/bin/mercury -v -S >/root/mercury/manual.log 2>&1 </dev/null
EOF
        sh_ "$ip" "sudo bash /root/deploy.sh" \
            || die "$ip: deploy of $branch failed (see /tmp/build.log there)"
    done

    log "waiting ${SETTLE}s for uucpd to re-attach"
    sleep "$SETTLE"

    for ip in "$SENDER_IP" "$RECEIVER_IP"; do
        local n
        n=$(sh_ "$ip" "pgrep -c mercury" 2>/dev/null || echo 0)
        [ "$n" = "1" ] || die "$ip: expected exactly 1 mercury, found $n"
    done
}

# ---- one timed transfer -----------------------------------------------------
run_one() {
    local build=$1 rep=$2

    # Stale queued jobs would be carried by this run's timing.
    sh_ "$SENDER_IP" "uustat -a 2>/dev/null | awk '{print \$1}' | xargs -r -n1 uustat -k" >/dev/null 2>&1

    local tag; tag="$(date +%s)-$build-$rep"
    sh_ "$SENDER_IP" "uucp $PAYLOAD '$RECEIVER_SYS!~/recv-$tag'" \
        || { log "queue failed ($build rep $rep)"; return 1; }

    local wall
    wall=$(sh_ "$SENDER_IP" "t0=\$(date +%s); sudo uucico -S $RECEIVER_SYS >/dev/null 2>&1; echo \$(( \$(date +%s) - t0 ))")
    [ -n "$wall" ] || { log "transfer produced no timing ($build rep $rep)"; return 1; }

    # Timing line the modem writes at disconnect.
    local tmg
    tmg=$(sh_ "$SENDER_IP" "grep -a 'arq-timing' /root/mercury/manual.log | tail -1")

    local tx frames retries snr
    tx=$(sed -n 's/.*tx_bytes=\([0-9]*\).*/\1/p'   <<<"$tmg")
    frames=$(sed -n 's/.*frames_tx=\([0-9]*\).*/\1/p' <<<"$tmg")
    retries=$(sed -n 's/.*retries=\([0-9]*\).*/\1/p'  <<<"$tmg")
    snr=$(sh_ "$RECEIVER_IP" "grep -aoE 'snr=[-0-9.]+' /root/mercury/manual.log | tail -1 | cut -d= -f2")

    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$build" "$(basename "$PAYLOAD")" "$rep" "$wall" \
        "${tx:-}" "${frames:-}" "${retries:-}" "${snr:-}" >> "$OUT"

    log "$build rep $rep: ${wall}s  tx=${tx:-?}B frames=${frames:-?} retries=${retries:-?} snr=${snr:-?}"
}

# ---- main -------------------------------------------------------------------
IFS=',' read -ra ORDER <<< "$BUILDS"
for b in "${ORDER[@]}"; do
    [ -n "${BRANCH[$b]:-}" ] || die "unknown build '$b' (have: ${!BRANCH[*]})"
done

preflight

[ -f "$OUT" ] || echo "build,payload,rep,wall_s,tx_bytes,frames_tx,retries,snr" > "$OUT"

for rep in $(seq 1 "$REPS"); do
    for b in "${ORDER[@]}"; do          # interleaved: drift hits every arm equally
        deploy "${BRANCH[$b]}"
        run_one "$b" "$rep" || log "run failed, continuing ($b rep $rep)"
    done
done

log "done -> $OUT"
echo
echo "medians (report these, not means — one failed connect skews a mean):"
awk -F, 'NR>1 && $4!="" {v[$1]=v[$1]" "$4}
         END{for (b in v){n=split(v[b],a," "); asort(a);
             printf "  %-10s %s s   (n=%d)\n", b, a[int((n+1)/2)], n}}' "$OUT" 2>/dev/null \
    || awk -F, 'NR>1{print "  "$1" "$4"s"}' "$OUT"
