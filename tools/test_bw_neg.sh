#!/bin/bash
# Test bandwidth negotiation loopback
# Usage: ./test_bw_neg.sh [cmd_mode] [rsp_mode]
#   cmd_mode/rsp_mode: "auto" or "nb" (default: both "auto")

CMD_MODE="${1:-auto}"
RSP_MODE="${2:-auto}"
MERCURY="C:/Program Files/Mercury/mercury.exe"
WAIT_SECS=100

echo "=== BW Negotiation Test: CMD=$CMD_MODE RSP=$RSP_MODE ==="

# Start responder
"$MERCURY" -p 7010 -i "CABLE Output" -o "CABLE Input" -M "$RSP_MODE" -R -g -Q 0 -v -n > /tmp/rsp_bwneg.log 2>&1 &
sleep 3

# Start commander
"$MERCURY" -p 7020 -i "CABLE Output" -o "CABLE Input" -M "$CMD_MODE" -R -g -Q 0 -v -n > /tmp/cmd_bwneg.log 2>&1 &
sleep 3

# Responder: set callsign and listen
(printf 'MYCALL RSP1\r'; sleep 1; printf 'LISTEN ON\r'; sleep $WAIT_SECS) | ncat 127.0.0.1 7010 > /dev/null 2>&1 &
sleep 2

# Commander: connect
(printf 'MYCALL CMD1\r'; sleep 1; printf 'CONNECT CMD1 RSP1\r'; sleep $WAIT_SECS) | ncat 127.0.0.1 7020 > /dev/null 2>&1 &

echo "Waiting ${WAIT_SECS}s for connection + turboshift..."
sleep $WAIT_SECS

echo ""
echo "=== COMMANDER LOG ==="
grep -E 'BW-NEG|SWITCH_BANDWIDTH|TURBO|CONNECTED|Bandwidth|NB-NEG|switch.*narrow|switch.*wide|capability' /tmp/cmd_bwneg.log 2>/dev/null || echo "(no BW-NEG lines)"
echo ""
echo "=== RESPONDER LOG ==="
grep -E 'BW-NEG|SWITCH_BANDWIDTH|TURBO|CONNECTED|Bandwidth|NB-NEG|switch.*narrow|switch.*wide|capability' /tmp/rsp_bwneg.log 2>/dev/null || echo "(no BW-NEG lines)"
echo ""
echo "=== FINAL STATUS ==="
echo "CMD:"
grep 'link_status' /tmp/cmd_bwneg.log | tail -3
echo "RSP:"
grep 'link_status' /tmp/rsp_bwneg.log | tail -3

# Show config to verify WB/NB
echo ""
echo "=== CONFIG ==="
echo "CMD:"
grep 'configuration:' /tmp/cmd_bwneg.log | tail -3
echo "RSP:"
grep 'configuration:' /tmp/rsp_bwneg.log | tail -3

taskkill //F //IM mercury.exe 2>/dev/null
echo ""
echo "=== DONE ==="
