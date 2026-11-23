#!/bin/sh
# Trace validation of SessionTeardown over the two-FSM sim: for each seed,
# tests/sim/teardown_trace writes TraceData.tla and TLC checks that the run is
# a behaviour of the spec (TraceTeardown.tla).
#
#   specs/teardown/trace_check.sh [first_seed] [last_seed] [per who sessions]
#
# Without the last three, each seed picks its own (teardown_trace).
#
# Prints a line a seed: "ok" (the whole trace matched) or "REJECTED after
# row k of n" (no behaviour of the spec goes on to row k+1), and keeps a
# rejected trace as TraceData_<seed>.tla.  Exits 1 if any was rejected.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
jar=${TLA2TOOLS:-/opt/TLA+Toolbox/tla2tools.jar}
meta=${TLC_METADIR:-${TMPDIR:-$HOME/.cache}/tlc-teardown-trace}
first=${1:-1}; last=${2:-$first}; shift 2 2>/dev/null; extra="$*"
make -s -C "$root/tests" teardown_trace || exit 2
mkdir -p "$meta"
cd "$here" || exit 2
bad=0
for seed in $(seq "$first" "$last"); do
    "$root/tests/teardown_trace" "$seed" $extra > TraceData.tla || exit 2
    args=$(sed -n 's/^\\\* teardown_trace //p' TraceData.tla)
    out=$(java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -workers 1 -metadir "$meta" \
              -config TraceTeardown.cfg TraceTeardown.tla 2>&1)
    furthest=$(echo "$out" | sed -n 's/.*<<"furthest row", \([0-9]*\), "of", \([0-9]*\)>>.*/\1 \2/p' | tail -1)
    if echo "$out" | grep -q "Invariant TraceNotDone is violated"; then
        echo "seed $seed ($args): ok, ${furthest#* } rows"
    elif [ -n "$furthest" ]; then
        echo "seed $seed ($args): REJECTED after row ${furthest% *} of ${furthest#* }"
        cp TraceData.tla "TraceData_$seed.tla"
        bad=1
    else
        echo "seed $seed: TLC failed"; echo "$out" | grep -A5 Error | head -20; exit 2
    fi
done
rm -f TraceData.tla
exit $bad
