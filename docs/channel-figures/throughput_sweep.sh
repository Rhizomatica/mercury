#!/bin/zsh
# One channel's column of the grid: 6 No x 2 sizes x 2 seeds
ARM=$1
for NO in -20 -24 -28 -32 -36 -40; do
  for PAYLOAD in 4096 32768; do
    for SEED in 1 2; do
      /tmp/opencode/effstudy/sweep_one.sh $ARM $NO $PAYLOAD $SEED
    done
  done
done
echo "WORKER $ARM COMPLETE" >> /tmp/opencode/effstudy/workers.log
