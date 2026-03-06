#!/bin/bash
# Test monitor mode data decode on VB-Cable.
# Launches: monitor (stdout), responder (port 7004), commander (port 7002)
# Feeds data via commander data port (7003), checks monitor output.

MERCURY='x:/Storage/Documents/hermes and mercury/mercury/mercury.exe'
# Use local build if present
if [ -f "./mercury.exe" ]; then MERCURY="./mercury.exe"; fi
AUDIO_IN="CABLE Output"
AUDIO_OUT="CABLE Input"
OUTDIR="/tmp/monitor_test_$$"
mkdir -p "$OUTDIR"

echo "Output dir: $OUTDIR"

# Kill any leftovers
taskkill //F //IM mercury.exe 2>/dev/null
sleep 2

# 1. Start monitor (passive, stdout output)
"$MERCURY" -m MONITOR -R -N -i "$AUDIO_IN" -o "$AUDIO_OUT" -x wasapi -p 7010 --stdout \
    > "$OUTDIR/monitor.log" 2>&1 &
MON_PID=$!
echo "Monitor started, PID=$MON_PID"

sleep 2

# 2. Start responder (port 7004)
"$MERCURY" -m ARQ -R -N -i "$AUDIO_IN" -o "$AUDIO_OUT" -x wasapi -p 7004 \
    > "$OUTDIR/responder.log" 2>&1 &
RSP_PID=$!
echo "Responder started, PID=$RSP_PID"

sleep 2

# 3. Set up responder: MYCALL + LISTEN ON
(printf "MYCALL W1TEST\r"; sleep 1; printf "LISTEN ON\r"; sleep 300) | ncat 127.0.0.1 7004 &
RSP_TCP=$!
echo "Responder listening"

sleep 3

# 4. Start commander (port 7002)
"$MERCURY" -m ARQ -R -N -i "$AUDIO_IN" -o "$AUDIO_OUT" -x wasapi -p 7002 \
    > "$OUTDIR/commander.log" 2>&1 &
CMD_PID=$!
echo "Commander started, PID=$CMD_PID"

sleep 2

# 5. Connect commander
(printf "MYCALL N0TEST\r"; sleep 1; printf "CONNECT N0TEST W1TEST\r"; sleep 300) | ncat 127.0.0.1 7002 &
CMD_TCP=$!
echo "Commander connecting..."

# 6. Poll for connection + turboshift completion
echo "Waiting for connection + turboshift..."
for i in $(seq 1 40); do
    sleep 5
    if grep -q "SWITCH_ROLE" "$OUTDIR/commander.log" 2>/dev/null; then
        CONFIG=$(grep "SET_CONFIG\|load_configuration\|config=" "$OUTDIR/commander.log" 2>/dev/null | tail -1)
        echo "  [$((i*5))s] Turboshift active. $CONFIG"
    fi
    if grep -q "is_at_top" "$OUTDIR/commander.log" 2>/dev/null; then
        echo "  [$((i*5))s] Turboshift complete (at ceiling)"
        break
    fi
done

echo ""
echo "=== Sending test data to commander data port 7003 ==="
# Send a larger test message that will trigger compression
TEST_MSG="MERCURY_MONITOR_DECODE_TEST"
FULL_MSG=""
for i in $(seq 1 20); do
    FULL_MSG="${FULL_MSG}${TEST_MSG}_LINE_${i}_THE_QUICK_BROWN_FOX_JUMPS_OVER_THE_LAZY_DOG\n"
done
# Keep connection alive with sleep so commander has time to read
(printf "$FULL_MSG"; sleep 60) | ncat 127.0.0.1 7003 &
DATA_PID=$!
echo "Data sent ($(printf "$FULL_MSG" | wc -c) bytes), ncat PID=$DATA_PID"

# 7. Wait for data to transit through the ARQ session
echo "Waiting for data transit (90s)..."
for i in $(seq 1 18); do
    sleep 5
    DATA_LINES=$(grep -c "RX-DECODE\|RX-DATA\|copy_data_to_buffer\|decompress" "$OUTDIR/monitor.log" 2>/dev/null || echo 0)
    MON_SIZE=$(wc -c < "$OUTDIR/monitor.log" 2>/dev/null || echo 0)
    echo "  [$((i*5))s] Monitor log: ${MON_SIZE} bytes, data events: $DATA_LINES"
    if grep -q "MERCURY_MONITOR" "$OUTDIR/monitor.log" 2>/dev/null; then
        echo "  *** PLAINTEXT DETECTED IN MONITOR OUTPUT ***"
        break
    fi
done

echo ""
echo "=== Results ==="
echo ""

echo "--- Monitor log (last 50 lines) ---"
tail -50 "$OUTDIR/monitor.log"
echo ""

echo "--- Commander data TX frames ---"
grep -c "CMD-TX" "$OUTDIR/commander.log" 2>/dev/null || echo "0"
grep "CMD-TX" "$OUTDIR/commander.log" 2>/dev/null | head -10
echo ""

echo "--- Monitor decoded frames ---"
grep -c "RX-DECODE" "$OUTDIR/monitor.log" 2>/dev/null || echo "0"
grep "RX-DECODE" "$OUTDIR/monitor.log" 2>/dev/null | tail -10
echo ""

echo "--- Monitor plaintext (non-debug lines) ---"
grep -v "^\[" "$OUTDIR/monitor.log" | grep -v "^$" | head -20
echo ""

echo "PIDs: monitor=$MON_PID responder=$RSP_PID commander=$CMD_PID"
echo "Logs in $OUTDIR/"
echo "Kill all: taskkill //F //IM mercury.exe"
