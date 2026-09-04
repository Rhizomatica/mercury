#!/bin/bash
# Notarize one artifact and staple the ticket to it.
#
# Submitting and waiting are deliberately separate operations.  rcodesign's
# combined `notary-submit --staple` aborts everything if a single poll request
# fails, and Apple's service is slow enough that an hour-long poll makes one
# bad request likely rather than unlucky.  Observed on CI:
#
#   waiting up to 3600s for package upload 80db453b... to finish processing
#   Error: error sending request for url (.../notary/v2/submissions/80db453b...)
#
# which threw away a 30 minute universal build for a submission Apple was
# still processing happily.  So: submit once, retry only the waiting, staple
# as its own step, and print the submission id so a lost run can be resumed
# rather than rebuilt.
#
# A rejection is NOT retried: "Invalid" is Apple's verdict, not a hiccup.
#
# usage: notarize.sh <artifact> <api-key-file> [max_wait] [retries] [submission_id]
set -u

ART=${1:?artifact path required}
KEY=${2:?api key file required}
WAIT=${3:-3600}
RETRIES=${4:-5}
SUB=${5:-}
RCODESIGN=${RCODESIGN:-rcodesign}

[ -e "$ART" ] || { echo "error: $ART does not exist"; exit 1; }
[ -f "$KEY" ] || { echo "error: notary key $KEY does not exist"; exit 1; }

# Apple's notary API takes an archive, not a bundle directory, so a .app is
# submitted as a ditto zip -- but the TICKET is stapled to the .app itself,
# never to the throwaway zip.
UPLOAD="$ART"
TMPZIP=""
case "$ART" in
  *.app)
    TMPZIP=$(mktemp -d)/$(basename "$ART").zip
    echo "archiving $(basename "$ART") for submission"
    ditto -c -k --keepParent "$ART" "$TMPZIP" || exit 1
    UPLOAD="$TMPZIP"
    ;;
esac
cleanup() { [ -n "$TMPZIP" ] && rm -rf "$(dirname "$TMPZIP")"; }
trap cleanup EXIT

if [ -z "$SUB" ]; then
    out=$("$RCODESIGN" notary-submit --api-key-file "$KEY" "$UPLOAD" 2>&1)
    echo "$out"
    SUB=$(printf '%s\n' "$out" | sed -n 's/^created submission ID: //p' | head -1)
fi
[ -n "$SUB" ] || { echo "error: no submission ID from notary-submit"; exit 1; }

echo "notarization submission: $SUB  ($(basename "$ART"))"
echo "  resume with: macos/notarize.sh '$ART' '$KEY' $WAIT $RETRIES $SUB"

n=0
while [ "$n" -lt "$RETRIES" ]; do
    n=$((n + 1))
    if w=$("$RCODESIGN" notary-wait --api-key-file "$KEY" --max-wait-seconds "$WAIT" "$SUB" 2>&1); then
        echo "$w"
        "$RCODESIGN" staple "$ART" || exit 1
        echo "  -> $ART (notarized + stapled)"
        exit 0
    fi
    echo "$w"
    if printf '%s\n' "$w" | grep -q "Invalid"; then
        echo "error: Apple REJECTED the submission -- not retrying"
        "$RCODESIGN" notary-log --api-key-file "$KEY" "$SUB" || true
        exit 1
    fi
    if [ "$n" -lt "$RETRIES" ]; then
        echo "notary-wait attempt $n/$RETRIES did not complete; retrying in 60s"
        sleep 60
    fi
done

echo "error: notarization did not complete for submission $SUB"
echo "  it may still be processing; resume with:"
echo "  macos/notarize.sh '$ART' '$KEY' $WAIT $RETRIES $SUB"
exit 1
