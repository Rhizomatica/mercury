#!/bin/bash
# tx_level_check.sh -- set and check TX drive on a real radio, one waveform at
# a time, before running Mercury's new waveforms through an amplifier.
#
# Usage:
#   utils/tx_level_check.sh [options] -- <radio options for mercury>
#
#   sBitx:  utils/tx_level_check.sh -- -S
#   ICOM:   utils/tx_level_check.sh -- -R 3073 -A /dev/ttyUSB0 -x alsa -i plughw:1,0 -o plughw:1,0
#           (3073 = IC-7300; `mercury -K` lists Hamlib models)
#
# Options:
#   -b <mercury>  binary to use (default: ./mercury)
#   -s <seconds>  how long to key each waveform (default 12, max 30)
#   -c <seconds>  cool-down between waveforms (default 30)
#   -a            all ladder modes, not just the three that bound the levels
#   -n            dry run: print what would be keyed, key nothing
#
# RUN IT INTO A DUMMY LOAD, with a wattmeter that shows average power, and
# watch ALC and PA temperature.
#
# Why these waveforms, in this order.  Measured at Mercury's output (TX gain
# 0 dB, true peak after 8x oversampling, i.e. what the soundcard reconstructs):
#
#   waveform            true peak   RMS      PAPR
#   MFSK / pattern ACK  -5.2 dBFS   -9.5     4.3 dB   <- most AVERAGE power
#   DATAC16 (control)   -5.9        -12.2    6.3 dB
#   QAM16C2 (fastest)   -6.5        -19.3    12.8 dB  <- least average power
#
# Peaks are within ~1 dB of each other, so no waveform is a peak-overdrive
# risk.  Average power is: MFSK puts out ~2.7 dB (about twice) the average
# power of DATAC16 at the same drive, for 13.5 s at a time -- like FSK/FT8.
# So set the drive on MFSK, at or below the radio's continuous-duty rating
# for digital modes; every other waveform then runs cooler.
#
# Each run is time-limited.  Mercury's TX test mode transmits back to back; it
# stops on the first SIGINT only in builds that carry the fix for it (the
# preflight below checks), and always finishes the frame in flight first --
# up to 13.5 s extra for MFSK.  Readings are appended to tx_level_check.csv.

set -u
MERCURY=./mercury
SECS=12
COOL=30
ALL=0
DRY=0
while getopts "b:s:c:an" o; do
    case $o in
        b) MERCURY=$OPTARG ;;
        s) SECS=$OPTARG ;;
        c) COOL=$OPTARG ;;
        a) ALL=1 ;;
        n) DRY=1 ;;
        *) sed -n '2,24p' "$0"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))
[ "${1:-}" = "--" ] && shift
RADIO_ARGS=("$@")

[ -x "$MERCURY" ] || { echo "no mercury binary at $MERCURY (use -b)"; exit 1; }
MERCURY=$(readlink -f "$MERCURY")
[ "$SECS" -le 30 ] 2>/dev/null || { echo "-s must be 30 s or less"; exit 1; }
[ ${#RADIO_ARGS[@]} -gt 0 ] || { echo "give the radio options after --, e.g. -- -S"; exit 1; }

# index:name:frame seconds:why
MODES=("11:MFSK:13.5:set the drive here -- highest average power"
       "8:DATAC16:3.7:control frames (CALL/ACCEPT/ACK)"
       "10:QAM16C2:3.7:fastest mode -- lowest average, highest PAPR")
[ $ALL = 1 ] && MODES+=("7:DATAC15:4.4:ladder" "3:DATAC4:5.8:ladder" "1:DATAC3:3.8:ladder"
                        "0:DATAC1:4.8:ladder" "9:DATAC17:7.4:ladder")

# Preflight, no radio involved: TX test mode must stop on ONE SIGINT.  An
# older build keeps keying after it, and only a second signal ends it,
# possibly with PTT still on.
echo "Preflight: checking that TX test mode stops on one SIGINT (no radio)..."
pf=$(mktemp -d)
( cd "$pf" && XDG_STATE_HOME=$pf exec "$MERCURY" -t -x null -m 8 -p 9990 -C "$pf/none.ini" ) \
    >"$pf/log" 2>&1 &
pid=$!
sleep 3
kill -INT $pid 2>/dev/null
for _ in $(seq 1 25); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then
    kill -INT $pid 2>/dev/null; sleep 1; kill -KILL $pid 2>/dev/null
    rm -rf "$pf"
    echo "ABORT: this mercury keeps transmitting after SIGINT in test mode."
    echo "       Use a build with 'stop TX test mode on a signal' (PR #296)."
    exit 1
fi
rm -rf "$pf"
echo "  ok"

LOG=tx_level_check.csv
[ -f $LOG ] || echo "date,radio_args,mode,seconds,avg_W,peak_W,alc,notes" > $LOG

echo
echo "Radio options: ${RADIO_ARGS[*]}"
echo "Each waveform keys for ${SECS} s plus the frame in flight; ${COOL} s cool-down between."
echo "DUMMY LOAD. Wattmeter on average. Watch ALC and PA temperature. Ctrl-C aborts."
for m in "${MODES[@]}"; do
    IFS=: read -r idx name frame why <<<"$m"
    echo
    echo "=== $name  (-m $idx, ${frame} s frames) -- $why"
    if [ $DRY = 1 ]; then
        echo "  [dry run] would run: timeout -s INT $SECS $MERCURY -t -m $idx ${RADIO_ARGS[*]}"
        continue
    fi
    read -r -p "  Press ENTER to key $name (or type s to skip): " ans
    [ "$ans" = "s" ] && continue
    start=$(date +%s)
    timeout -s INT "$SECS" "$MERCURY" -t -m "$idx" "${RADIO_ARGS[@]}" >/dev/null 2>&1
    echo "  unkeyed after $(( $(date +%s) - start )) s"
    read -r -p "  Average W: " avg
    read -r -p "  Peak W:    " pk
    read -r -p "  ALC (none/light/heavy): " alc
    read -r -p "  Notes:     " notes
    echo "$(date -Is),\"${RADIO_ARGS[*]}\",$name,$SECS,$avg,$pk,$alc,\"$notes\"" >> $LOG
    echo "  cooling ${COOL} s..."; sleep "$COOL"
done
echo
echo "Done. Readings in $LOG."
echo "Expected, at one drive setting: MFSK average about 2x DATAC16's, and about"
echo "10x QAM16C2's; peaks within ~1 dB of each other."
