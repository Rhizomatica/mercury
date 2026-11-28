#!/bin/sh
# Run TLC on the TLA+ models under specs/ and check each result against the
# one its README promises (specs/tlc.list).  Used by `make tla` and
# `make tla-quick`.
#
#   specs/tlc.sh [quick|all] [pattern]
#
#   quick    the configurations marked quick (~6 min in all on 4 workers)
#   all      quick and long ones: hours
#   pattern  only the lines of tlc.list that contain it, e.g. turns/ or Mut_;
#            a huge configuration (not exhausted, or about a day) runs only
#            when a pattern selects it
#
# Environment:
#   TLA2TOOLS    path to tla2tools.jar (default /opt/TLA+Toolbox/tla2tools.jar)
#   JAVA         the java to run (default java)
#   TLC_WORKERS  TLC worker threads per run (default 4)
#   TLC_JAVA_OPTS  options for the JVM (default -Xmx4g -XX:+UseParallelGC)
#   TLC_OUT      where TLC's state and each run's output go (default
#                specs/.tlc, ignored by git)
#
# Each line of tlc.list is `dir config spec expect class [tlc flags...]`:
# `expect` is `ok` (no error), `deadlock`, or the name of the invariant or
# property TLC must report violated (the mutants); `class` is quick or long.
# Exits non-zero if any result differs from its line, or TLC did not finish.
set -u
here=$(cd "$(dirname "$0")" && pwd)
mode=${1:-quick}
pat=${2:-}
case $mode in quick|all) ;; *) echo "usage: $0 [quick|all] [pattern]" >&2; exit 2;; esac
jar=${TLA2TOOLS:-/opt/TLA+Toolbox/tla2tools.jar}
java=${JAVA:-java}
workers=${TLC_WORKERS:-4}
jopts=${TLC_JAVA_OPTS:--Xmx4g -XX:+UseParallelGC}
out=${TLC_OUT:-$here/.tlc}
[ -f "$jar" ] || { echo "tla2tools.jar not found at $jar (set TLA2TOOLS)" >&2; exit 2; }
command -v "$java" >/dev/null 2>&1 || { echo "$java not found (set JAVA)" >&2; exit 2; }
mkdir -p "$out"

pass=0; fail=0; failed=""
while read -r dir cfg spec expect class flags; do
    case $dir in ''|'#'*) continue;; esac
    [ "$mode" = quick ] && [ "$class" != quick ] && continue
    [ "$class" = huge ] && [ -z "$pat" ] && continue
    [ -n "$pat" ] && case "$dir/$cfg $spec" in *"$pat"*) ;; *) continue;; esac
    name=$dir-$cfg
    log=$out/$name.out
    rm -rf "$out/meta-$name"
    t0=$(date +%s)
    # shellcheck disable=SC2086
    (cd "$here/$dir" && "$java" $jopts -Djava.io.tmpdir="$out" -cp "$jar" tlc2.TLC \
        -workers "$workers" -noGenerateSpecTE -metadir "$out/meta-$name" $flags \
        -config "$cfg.cfg" "$spec.tla") < /dev/null > "$log" 2>&1
    secs=$(( $(date +%s) - t0 ))
    rm -rf "$out/meta-$name"
    if grep -q 'No error has been found' "$log"; then got=ok
    elif grep -q 'Error: Deadlock reached' "$log"; then got=deadlock
    else
        got=$(sed -n -e 's/^Error: Invariant \([A-Za-z0-9_]*\) is violated.*/\1/p' \
                     -e 's/^Error: Action property \([A-Za-z0-9_]*\) is violated.*/\1/p' \
                     -e 's/^Error: Temporal property \([A-Za-z0-9_]*\) was violated.*/\1/p' "$log" | head -1)
        [ -n "$got" ] || got="no-result"
    fi
    states=$(sed -n 's/.* \([0-9][0-9]*\) distinct states found.*/\1/p' "$log" | tail -1)
    if [ "$got" = "$expect" ]; then
        pass=$((pass + 1)); verdict=PASS
    else
        fail=$((fail + 1)); verdict=FAIL; failed="$failed $dir/$cfg"
    fi
    printf '%s %-40s %-22s want %-18s got %-18s %8s states %5ss\n' \
        "$verdict" "$dir/$cfg" "$spec" "$expect" "$got" "${states:-?}" "$secs"
done < "$here/tlc.list"

echo "TLC: $pass passed, $fail failed (output in $out)"
[ "$fail" -eq 0 ] || { echo "failed:$failed" >&2; exit 1; }
