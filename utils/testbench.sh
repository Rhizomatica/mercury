#!/bin/bash
# testbench.sh — one entry point that builds, runs and logs the whole
# validation battery, with enough provenance to still be interpretable in six
# months.
#
#   ./utils/testbench.sh --builds v1.9.10,mercuryv2,arq-windowed \
#                        --stages unit,loopsim --reps 3
#
# Everything lands in results/<timestamp>/:
#   MANIFEST.txt   host, kernel, CPU, and per build: branch/tag, commit,
#                  dirty-tree flag, binary sha256
#   results.csv    one row per run, every stage
#   SUMMARY.md     medians per build per cell
#   logs/          per-run logs, named <stage>-<build>-<cell>-<rep>.log
#
# Why this exists: the pieces were all manual and scattered, results were not
# collected anywhere, and every comparison was re-orchestrated by hand — which
# is how a whole evening produced one unusable column. A result you cannot tie
# to a commit and a host is not a result.
#
# Stages
#   unit     : make test (unit + sim + integration) — fast, no radio
#   loopsim  : two real mercury instances over ALSA snd-aloop, clean and
#              asymmetric cells. Real-time: needs a QUIET host.
#   sock     : skywave mercury_sock bakeoff (virtual clock, pace-invariant, so
#              it tolerates a busy host). Needs the skywave checkout.
#   bench    : dummy-load run on the two stations (delegates to
#              dummyload_ab.sh). Needs the radios.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILDS="mercuryv2,arq-windowed"
STAGES="unit,loopsim"
REPS=3
PAYLOAD=5120
OUTBASE="$REPO/results"
SKYWAVE=${SKYWAVE:-/home/rafael2k/files/rhizomatica/hermes/skywave}

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \?//'
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --builds)  BUILDS=$2; shift 2 ;;
        --stages)  STAGES=$2; shift 2 ;;
        --reps)    REPS=$2; shift 2 ;;
        --payload) PAYLOAD=$2; shift 2 ;;
        --out)     OUTBASE=$2; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1"; usage ;;
    esac
done

RUN="$OUTBASE/$(date +%Y%m%d-%H%M%S)"
LOGS="$RUN/logs"
BINS="$RUN/bins"
CSV="$RUN/results.csv"
mkdir -p "$LOGS" "$BINS"

say()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$RUN/testbench.log"; }
warn() { printf '[%s] WARNING: %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$RUN/testbench.log" >&2; }

has_stage() { [[ ",$STAGES," == *",$1,"* ]]; }

echo "stage,build,cell,rep,metric,value,status" > "$CSV"

# ---------------------------------------------------------------------------
# Provenance. Without this a CSV is just numbers with no way back to the code
# that produced them.
# ---------------------------------------------------------------------------
{
    echo "# testbench run $(basename "$RUN")"
    echo "date_utc:  $(date -u +%FT%TZ)"
    echo "host:      $(hostname)"
    echo "kernel:    $(uname -sr)"
    echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs)"
    echo "cores:     $(nproc)"
    echo "loadavg:   $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "stages:    $STAGES"
    echo "builds:    $BUILDS"
    echo "reps:      $REPS"
    echo "payload:   $PAYLOAD"
} > "$RUN/MANIFEST.txt"

# ---------------------------------------------------------------------------
# Build each ref into $BINS, recording exactly what was built.
# ---------------------------------------------------------------------------
build_one() {
    local ref=$1 bin="$BINS/mercury-$1" wt="$RUN/wt-$1"

    if git -C "$REPO" rev-parse --verify -q "$ref" >/dev/null; then
        local sha; sha=$(git -C "$REPO" rev-parse --short "$ref")
        # A worktree keeps the caller's checkout untouched — an earlier version
        # of this ritual switched branches under a running test.
        git -C "$REPO" worktree add -q --detach "$wt" "$ref" 2>/dev/null || {
            warn "worktree for $ref failed; skipping build"; return 1; }
        ( cd "$wt" && make -j"$(nproc)" ) > "$LOGS/build-$ref.log" 2>&1 || {
            warn "build of $ref FAILED (see logs/build-$ref.log)"; return 1; }
        cp "$wt/mercury" "$bin"
        WORKTREES+=("$wt")
        printf 'build %-22s ref=%s commit=%s sha256=%s\n' \
            "$ref" "$ref" "$sha" "$(sha256sum "$bin" | cut -c1-16)" >> "$RUN/MANIFEST.txt"
        say "built $ref ($sha)"
        return 0
    fi
    warn "unknown git ref: $ref"
    return 1
}

# Worktrees are removed at exit: they are build scratch, and leaving them
# behind pollutes `git worktree list` and the disk after every run.
WORKTREES=()
cleanup_worktrees() {
    local wt
    for wt in "${WORKTREES[@]:-}"; do
        [ -n "$wt" ] && git -C "$REPO" worktree remove --force "$wt" >/dev/null 2>&1
    done
    git -C "$REPO" worktree prune >/dev/null 2>&1
}
trap cleanup_worktrees EXIT

IFS=',' read -ra BUILD_LIST <<< "$BUILDS"
BUILT=()
for b in "${BUILD_LIST[@]}"; do
    build_one "$b" && BUILT+=("$b")
done
[ ${#BUILT[@]} -gt 0 ] || { warn "nothing built; stopping"; exit 1; }

# ---------------------------------------------------------------------------
# Stage: unit — the deterministic gate. Runs on the current tree only; it tests
# source, not binaries.
# ---------------------------------------------------------------------------
if has_stage unit; then
    say "stage unit: make test"
    if ( cd "$REPO/tests" && make test ) > "$LOGS/unit.log" 2>&1; then
        say "  unit: PASS"
        echo "unit,current,-,1,suite,pass,ok" >> "$CSV"
    else
        warn "  unit: FAIL (see logs/unit.log)"
        echo "unit,current,-,1,suite,fail,fail" >> "$CSV"
    fi
fi

# ---------------------------------------------------------------------------
# Stage: loopsim — two real instances over the kernel loopback.
#
# REAL TIME: its numbers are corrupted by a busy host, unlike the virtual-clock
# sock stage. Do not run it alongside anything heavy.
# ---------------------------------------------------------------------------
loopsim_cleanup() {
    pkill -9 -x mercury            >/dev/null 2>&1
    pkill -9 -f loopsim/noisebridge >/dev/null 2>&1
    pkill -9 -f 'arecord -D plughw' >/dev/null 2>&1
    sleep 1
}

loopsim_ready() {
    # Poll the two control ports. A fixed sleep raced the instances binding and
    # made a startup race look exactly like a modem failure.
    local i
    for i in $(seq 1 40); do
        if timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/8300" 2>/dev/null &&
           timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/8400" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

if has_stage loopsim; then
    if ! aplay -l 2>/dev/null | grep -qi "Loopback"; then
        warn "stage loopsim SKIPPED: no snd-aloop card (sudo modprobe snd-aloop)"
    else
        make -C "$REPO/utils/loopsim" >/dev/null 2>&1
        say "stage loopsim: ${#BUILT[@]} builds x $REPS reps (real-time — keep the host quiet)"

        # cell name -> "FWD REV" noise. Clean plus the asymmetric case that
        # breaks ACK survival on real HF.
        declare -A CELLS=( [clean]="0.0 0.0" [asym-rev1.0]="0.0 1.0" )

        for rep in $(seq 1 "$REPS"); do
            for cell in "${!CELLS[@]}"; do
                for b in "${BUILT[@]}"; do          # interleaved: drift hits every arm
                    read -r fwd rev <<< "${CELLS[$cell]}"
                    loopsim_cleanup
                    ( cd "$REPO/utils/loopsim" &&
                      MERCURY="$BINS/mercury-$b" ./run_loopsim.sh "$fwd" "$rev" ) \
                        > "$LOGS/loopsim-$b-$cell-$rep.log" 2>&1

                    if ! loopsim_ready; then
                        warn "  $b/$cell rep$rep: stations never came up"
                        echo "loopsim,$b,$cell,$rep,bps,0,nostart" >> "$CSV"
                        cp /tmp/mA.log "$LOGS/loopsim-$b-$cell-$rep.mA.log" 2>/dev/null
                        cp /tmp/mB.log "$LOGS/loopsim-$b-$cell-$rep.mB.log" 2>/dev/null
                        continue
                    fi
                    sleep 2

                    local_res=$( cd "$REPO/utils/loopsim" &&
                                 PAYLOAD=$PAYLOAD timeout 400 python3 drive.py 2>&1 | tail -3 )
                    echo "$local_res" >> "$LOGS/loopsim-$b-$cell-$rep.log"
                    # Keep the modem logs: the transfer number alone never
                    # explains a bad run.
                    cp /tmp/mA.log "$LOGS/loopsim-$b-$cell-$rep.mA.log" 2>/dev/null
                    cp /tmp/mB.log "$LOGS/loopsim-$b-$cell-$rep.mB.log" 2>/dev/null

                    # drive.py prints exactly:
                    #   === RESULT: <got>/<total> bytes in <s>s (<bps> bps) match=<True|False> ===
                    line=$(grep -o '=== RESULT:.*===' <<<"$local_res" | tail -1)
                    got=$(sed -n 's/.*RESULT: \([0-9]*\)\/.*/\1/p'      <<<"$line")
                    bps=$(sed -n 's/.*(\([0-9.]*\) bps).*/\1/p'          <<<"$line")
                    match=$(sed -n 's/.*match=\([A-Za-z]*\).*/\1/p'      <<<"$line")

                    # A transfer that finishes with the wrong bytes is a FAILURE,
                    # not a slow success: report it as such rather than logging a
                    # healthy-looking bps for corrupt data.
                    if [ -z "$line" ]; then           st=norun
                    elif [ "$match" != "True" ]; then st=mismatch
                    elif [ "${got:-0}" -lt "$PAYLOAD" ] 2>/dev/null; then st=partial
                    else                              st=ok
                    fi
                    echo "loopsim,$b,$cell,$rep,bps,${bps:-0},$st" >> "$CSV"
                    say "  $b/$cell rep$rep: ${bps:-0} bps ${got:-0}/$PAYLOAD B match=${match:-?} $st"
                done
            done
        done
        loopsim_cleanup
    fi
fi

# ---------------------------------------------------------------------------
# Stage: sock — skywave virtual-clock bakeoff. Pace-invariant, so a busy host
# does not corrupt it. Note 1.9.9/1.9.10 CANNOT run here: -x sock landed after
# 1.9.10, which is why loopsim is the instrument for released-version A/Bs.
# ---------------------------------------------------------------------------
if has_stage sock; then
    if [ ! -d "$SKYWAVE/src/skywave" ]; then
        warn "stage sock SKIPPED: no skywave checkout at $SKYWAVE (set SKYWAVE=)"
    else
        say "stage sock: skywave mercury_sock"
        spec="$RUN/cells-sock.json"
        cat > "$spec" <<EOF
[{"sigma":0,"watterson":"off","payload":$PAYLOAD,"timeout":600,"reps":$REPS},
 {"sigma":4000,"watterson":"off","payload":$PAYLOAD,"timeout":600,"reps":$REPS},
 {"sigma":4000,"watterson":"moderate","payload":$PAYLOAD,"timeout":600,"reps":$REPS}]
EOF
        for b in "${BUILT[@]}"; do
            ( cd "$SKYWAVE" && PYTHONPATH=src MERCURY_BIN="$BINS/mercury-$b" \
                python3 -m skywave.sweep_runner mercury_sock "$spec" \
                        "$RUN/sock-$b.csv" "$b" ) > "$LOGS/sock-$b.log" 2>&1
            # Fold the runner's own CSV into ours.
            tail -n +2 "$RUN/sock-$b.csv" 2>/dev/null | awk -F, -v b="$b" \
                '{printf "sock,%s,s%s-%s,%s,goodput,%s,%s\n", b, $3, $7, $9, $13, $17}' >> "$CSV"
            say "  sock/$b done"
        done
    fi
fi

# ---------------------------------------------------------------------------
# Stage: bench — real radios on dummy loads.
# ---------------------------------------------------------------------------
if has_stage bench; then
    say "stage bench: dummy-load stations (see docs/DUMMY-LOAD-TEST-PLAN.md)"
    "$REPO/utils/dummyload_ab.sh" --reps "$REPS" --out "$RUN/bench.csv" \
        > "$LOGS/bench.log" 2>&1 \
        || warn "  bench stage reported failures (see logs/bench.log)"
    tail -n +2 "$RUN/bench.csv" 2>/dev/null | awk -F, \
        '{printf "bench,%s,%s,%s,wall_s,%s,ok\n", $1, $2, $3, $4}' >> "$CSV"
fi

# ---------------------------------------------------------------------------
# Summary: medians, because one failed connect ruins a mean.
# ---------------------------------------------------------------------------
{
    echo "# Testbench summary — $(basename "$RUN")"
    echo
    sed -n '2,12p' "$RUN/MANIFEST.txt" | sed 's/^/    /'
    echo
    echo "| stage | build | cell | n | median | status mix |"
    echo "|---|---|---|---|---|---|"
    awk -F, 'NR>1 {
        key=$1"|"$2"|"$3; vals[key]=vals[key]" "$6; st[key]=st[key]" "$7 }
      END {
        for (k in vals) {
          n=split(vals[k], a, " "); m=0
          # tiny insertion sort: the arrays here are a handful of entries
          for (i=1;i<=n;i++) for (j=i+1;j<=n;j++) if (a[j]+0<a[i]+0) {t=a[i];a[i]=a[j];a[j]=t}
          if (n>0) m=a[int((n+1)/2)]
          split(k, p, "|")
          printf "| %s | %s | %s | %d | %s | %s |\n", p[1], p[2], p[3], n, m, st[k]
        } }' "$CSV" | sort
    echo
    echo "Medians, not means. Numbers from the \`loopsim\` and \`bench\` stages are"
    echo "real-time and only comparable to others taken on a similarly quiet host."
} > "$RUN/SUMMARY.md"

say "done"
echo
cat "$RUN/SUMMARY.md"
echo
echo "full results: $RUN"
