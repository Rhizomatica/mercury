#!/bin/sh
# Upload an .aab to Google Play and release it on a track, through the Play
# Developer API with a service account (release.yml, job "play").
#
#   play_upload.sh <service-account.json> <package> <bundle.aab> <track> [notes-file]
#   play_upload.sh --check <service-account.json> <package>
#
# <track> is internal, alpha (closed testing), beta (open testing) or
# production; the release goes out to all of the track's users at once.
# notes-file holds the release notes as Play Console takes them,
# <en-US>...</en-US> per language.  --check only opens an edit, lists the
# tracks and discards it: it proves the key and its Play permissions.
# Needs openssl, curl and jq.  The key and the token stay in files (mode 600),
# never on a command line.
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
check=0
if [ "${1:-}" = "--check" ]; then check=1; shift; fi
[ $# -ge 2 ] || { sed -n '2,15p' "$0" >&2; exit 2; }
SA=$1 PKG=$2
if [ $check = 0 ]; then
    [ $# -ge 4 ] || { sed -n '2,15p' "$0" >&2; exit 2; }
    AAB=$3 TRACK=$4 NOTES=${5:-}
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
umask 077

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

# An OAuth token from a JWT signed with the service account's key.
jq -r .private_key "$SA" > "$tmp/key.pem"
now=$(date +%s)
hdr=$(printf '{"alg":"RS256","typ":"JWT"}' | b64url)
claims=$(jq -cn --arg iss "$(jq -r .client_email "$SA")" --argjson now "$now" \
    '{iss: $iss, scope: "https://www.googleapis.com/auth/androidpublisher",
      aud: "https://oauth2.googleapis.com/token", iat: $now, exp: ($now + 3600)}' | b64url)
sig=$(printf '%s.%s' "$hdr" "$claims" | openssl dgst -sha256 -sign "$tmp/key.pem" -binary | b64url)
printf 'grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=%s.%s.%s' \
    "$hdr" "$claims" "$sig" > "$tmp/assertion"
curl -sS --fail-with-body -d @"$tmp/assertion" https://oauth2.googleapis.com/token > "$tmp/token.json"
printf 'Authorization: Bearer %s\n' "$(jq -r .access_token "$tmp/token.json")" > "$tmp/auth"
rm -f "$tmp/key.pem" "$tmp/assertion" "$tmp/token.json"

API=https://androidpublisher.googleapis.com/androidpublisher/v3/applications/$PKG
UP=https://androidpublisher.googleapis.com/upload/androidpublisher/v3/applications/$PKG
call() { curl -sS --fail-with-body -H @"$tmp/auth" "$@"; }

edit=$(call -X POST "$API/edits" | jq -r .id)

if [ $check = 1 ]; then
    call "$API/edits/$edit/tracks" | jq -r '.tracks[] | "\(.track): \([.releases[]? | "\(.name // "-") [\(.status)] \(.versionCodes // [] | join(","))"] | join("; "))"'
    call -X DELETE "$API/edits/$edit" > /dev/null
    exit 0
fi

vc=$(call -H "Content-Type: application/octet-stream" --data-binary @"$AAB" \
    "$UP/edits/$edit/bundles?uploadType=media" | jq -r .versionCode)
echo "uploaded versionCode $vc"

# The notes file's <xx-YY>...</xx-YY> blocks as Play's releaseNotes list.
notes='[]'
if [ -n "$NOTES" ]; then
    notes=$(jq -Rs '[scan("<(?<l>[A-Za-z0-9-]+)>\\n?(?<t>[^<]*)</[A-Za-z0-9-]+>") |
        {language: .[0], text: (.[1] | sub("\\s+$"; ""))}]' "$NOTES")
fi
jq -n --arg track "$TRACK" --arg vc "$vc" --argjson notes "$notes" \
    '{track: $track, releases: [{versionCodes: [$vc], status: "completed", releaseNotes: $notes}]}' \
    > "$tmp/track.json"
call -X PUT -H "Content-Type: application/json" --data @"$tmp/track.json" \
    "$API/edits/$edit/tracks/$TRACK" > /dev/null
call -X POST "$API/edits/$edit:commit" > /dev/null
echo "released versionCode $vc on $TRACK"
