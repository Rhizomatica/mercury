#!/bin/bash
# sign.sh — Authenticode-sign one or more Windows binaries IN PLACE with the
# Certum SimplySign cloud certificate.  Logs in once (idempotent) and reuses the
# session across every argument and across repeated invocations, so the
# Makefile's per-file calls cost a single cloud login.  Run sign-logout.sh when
# the release is finished.  See docs/WINDOWS-SIGNING.md (secrets go OUTSIDE the
# repo; point CERTUM_EMAIL / CERTUM_OTP_URI_FILE at them).
#
#   CERTUM_EMAIL=you@example.org CERTUM_OTP_URI_FILE=~/secrets/otpauth.txt \
#     ./sign.sh mercury.exe mercury-ui.exe
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sign-lib.sh
source "$DIR/sign-lib.sh"
[ "$#" -ge 1 ] || { echo "usage: $0 <file.exe> [more.exe ...]" >&2; exit 2; }
rc=0
for f in "$@"; do ss_sign_file "$f" || rc=1; done
exit "$rc"
