#!/bin/bash
# One channel's column of the grid: 6 No x 2 sizes x 2 seeds
# Usage: throughput_sweep.sh <arm>     (awgn, ccir-good, ccir-poor)
ARM=$1
HERE="$(cd "$(dirname "$0")" && pwd)"
for NO in -20 -24 -28 -32 -36 -40; do
  for PAYLOAD in 4096 32768; do
    for SEED in 1 2; do
      "$HERE/throughput_sweep_cell.sh" $ARM $NO $PAYLOAD $SEED
    done
  done
done
echo "WORKER $ARM COMPLETE"
