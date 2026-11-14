#!/bin/zsh
# Phase 2: airtime accounting.
# CSV: chan,traffic,seed,intact,done_s,air_a,air_b,keys,ofdm_air,ctl_air,data_air,floor_residual
cd /home/nkgoodall/projects/mercury/tests
B=./carousel_bench
OUT=/tmp/opencode/effstudy/accounting.csv
SC_SPARSE='A30+0,B30+9000,A30+18000,B30+27000,A30+36000'
SC_DENSE='A2500+1000,B300+2000,A2000+4000,B300+6000,A2000+8000,B300+10000,A1000+12000,B300+14000'
for chan in awgn:0.02 cliff:8 cliff:3 fade:8:0.5 fade:3:0.5 nvis; do
  for traffic in bulk sparse dense; do
    for seed in $(seq 1 10); do
      case $traffic in
        bulk)   OUTL=$(SIM_STATS=1 CAR_LIMIT_S=7200 $B $seed $chan 2>&1);;
        sparse) OUTL=$(SIM_STATS=1 CAR_LIMIT_S=7200 CAR_SCRIPT="$SC_SPARSE" $B $seed $chan 2>&1);;
        dense)  OUTL=$(SIM_STATS=1 CAR_LIMIT_S=7200 CAR_SCRIPT="$SC_DENSE" $B $seed $chan 2>&1);;
      esac
      python3 /tmp/opencode/effstudy/acct_parse.py "$chan" "$traffic" "$seed" <<< "$OUTL" >> $OUT
    done
  done
done
echo "ACCOUNTING COMPLETE" >> /tmp/opencode/effstudy/workers.log
