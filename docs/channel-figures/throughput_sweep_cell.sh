#!/bin/zsh
# Efficiency study Phase 1: throughput grid on the watterson engine.
# One CSV line per run: arm,No,payload,seed,result,wall_s,connect_s,bytes_per_s
# Usage: sweep_one.sh <arm> <No> <payload> <seed>
set -e
cd /home/nkgoodall/projects/mercury/tests/integration
ARM=$1; NO=$2; PAYLOAD=$3; SEED=$4
OUT=/tmp/opencode/effstudy
LOG=$OUT/log_${ARM}_n${NO}_p${PAYLOAD}_s${SEED}.txt
START=$(date +%s)

export MERCURY_CH_ENGINE=watterson
if [ "$ARM" = "awgn" ]; then unset MERCURY_CH_FADING; else export MERCURY_CH_FADING=$ARM; fi
export MERCURY_CH_NO=$NO
export MERCURY_TEST_PAYLOAD_B=$PAYLOAD
export MERCURY_TEST_SEED=$SEED
unset MERCURY_CH_NO_FWD MERCURY_CH_NO_REV

RESULT=timeout
CONNECT=""
GOTEST=""
if timeout -k 30 2700 go test -run 'TestMercuryARQTransfer$' -v -count=1 -timeout 40m > $LOG 2>&1; then
  RESULT=pass
else
  grep -q "^--- FAIL\|^--- PASS" $LOG && RESULT=$(grep -oE "^--- (FAIL|PASS)" $LOG | head -1 | awk '{print $2}')
fi
END=$(date +%s)
WALL=$((END - START))
# go test's own measure of the test (the date wall can be held open by the
# session's grandchildren after the test has passed)
GOTEST=$(grep -oE "\(tests? |\)[0-9.]+s\)" $LOG | head -1)
GOTEST=$(grep -oE -- "--- (PASS|FAIL): TestMercuryARQTransfer \([0-9.]+s\)" $LOG | grep -oE "[0-9.]+s" | head -1)
WALL=${GOTEST%%s}
WALL=${WALL:-$((END - START))}
WALL=${WALL%.*}
CONNECT=$(grep -oE "CONNECTED after [0-9.]+ s" $LOG | grep -oE "[0-9.]+" | head -1)
BPS=$(python3 -c "print(round(${PAYLOAD}/${WALL},2))" 2>/dev/null || echo 0)
echo "${ARM},${NO},${PAYLOAD},${SEED},${RESULT},${WALL},${CONNECT:-0},${BPS}" >> $OUT/throughput.csv
echo "DONE ${ARM} ${NO} ${PAYLOAD} s${SEED}: ${RESULT} wall=${WALL}s connect=${CONNECT:-?}s"
