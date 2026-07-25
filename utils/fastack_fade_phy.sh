#!/bin/bash
# fastack_fade_phy.sh — deterministic PHY-robustness sweep for the fast-windowed
# ACK epoch pattern under Watterson fading.  For each SNR it fades N independent
# pattern segments (streaming fade => each segment is a fresh realisation) and
# reports the two decisive rates:
#   tagged_ok%    — tagged pattern recovered with the RIGHT epoch (speed)
#   false_tag%    — BARE pattern mis-read as tagged (SAFETY — must stay ~0)
# plus tagged miss/misdecode (graceful-degradation shape).
#
# Channel = utils/watterson_test (Watterson, matches freedv ch.c).  NOTE: the
# reported SNR3k is FILE-AVERAGED over 1/3-duty segments and the tone pattern
# reads ~6 dB hotter than a spread modem frame, so the ABSOLUTE axis is soft;
# the SHAPE (where tagged falls off, whether false_tag ever rises) is the point.
#
#   PROBE=/tmp/pattern_fade_probe utils/fastack_fade_phy.sh [preset] [N]
set -uo pipefail
cd "$(dirname "$0")/.."
PROBE="${PROBE:-/tmp/pattern_fade_probe}"
WT=utils/watterson_test
PRESET="${1:-poor}"           # good|moderate|poor|flutter
N="${2:-200}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

TAGGED_KINDS=(0x80 0x82 0x84 0x86)   # epoch 0..3, break=0
BARE_KIND=0x00

sum_field(){ grep -oE "$1=[0-9]+" | grep -oE '[0-9]+$' | paste -sd+ | bc; }

echo "# fast-ACK epoch pattern — Watterson '$PRESET' fade, N=$N per point"
printf "%-9s %-9s | %-9s %-10s %-8s | %-10s %-8s\n" \
       "No" "SNR3k" "tagged_ok" "misdecode" "miss" "false_tag" "bare_miss"
for No in ${NO_LIST:- -30 -26 -22 -20 -18 -16 -14 -12 -10 -8}; do
  # tagged: pool the 4 epochs
  tot=0; ok=0; mis=0; tmiss=0
  for k in "${TAGGED_KINDS[@]}"; do
    "$PROBE" emit "$k" "$N" "$TMP/e.raw"
    snr=$("$WT" "$TMP/e.raw" "$TMP/ef.raw" --"$PRESET" --No $No 2>&1 | grep -oE "SNR3k\(dB\):[ ]+[-0-9.]+" | grep -oE "[-0-9.]+$")
    line=$("$PROBE" detect "$k" "$N" "$TMP/ef.raw")
    ok=$((  ok  + $(grep -oE "correct=[0-9]+"   <<<"$line" | grep -oE '[0-9]+') ))
    mis=$(( mis + $(grep -oE "misdecode=[0-9]+" <<<"$line" | grep -oE '[0-9]+') ))
    tmiss=$(( tmiss + $(grep -oE " miss=[0-9]+" <<<"$line" | grep -oE '[0-9]+') ))
    tot=$(( tot + N ))
  done
  # bare: false-tag rate
  "$PROBE" emit "$BARE_KIND" "$N" "$TMP/b.raw"
  "$WT" "$TMP/b.raw" "$TMP/bf.raw" --"$PRESET" --No $No >/dev/null 2>&1
  bline=$("$PROBE" detect "$BARE_KIND" "$N" "$TMP/bf.raw")
  ftag=$(grep -oE "false_tag=[0-9]+" <<<"$bline" | grep -oE '[0-9]+')
  bmiss=$(grep -oE " miss=[0-9]+"     <<<"$bline" | grep -oE '[0-9]+')
  printf "%-9s %-9s | %-9s %-10s %-8s | %-10s %-8s\n" \
    "$No" "${snr:-?}" \
    "$(awk "BEGIN{printf \"%.1f%%\", 100*$ok/$tot}")" \
    "$(awk "BEGIN{printf \"%.1f%%\", 100*$mis/$tot}")" \
    "$(awk "BEGIN{printf \"%.1f%%\", 100*$tmiss/$tot}")" \
    "$(awk "BEGIN{printf \"%.1f%%\", 100*$ftag/$N}")" \
    "$(awk "BEGIN{printf \"%.1f%%\", 100*$bmiss/$N}")"
done
