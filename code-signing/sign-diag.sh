#!/bin/bash
# sign-diag.sh — tell me WHY SimplySign signing won't work here, before (or
# instead of) burning a release build on it.
#
# The login is a blind GUI drive on a virtual X server, so a broken environment
# used to surface many minutes later as one unhelpful line:
#     [ssign] ERROR: token did not come online (login likely failed)
# ...which is equally consistent with a missing package, a display collision, a
# skewed clock, a stale TOTP seed, and a genuinely rejected password.  This
# script checks each of those separately and names the one that is wrong.
#
#   ./sign-diag.sh            environment report only — touches no cloud service
#   ./sign-diag.sh --login    also attempt a real login, snapshotting every step
#
# Never prints a secret: the e-mail is masked and the TOTP is only shape-checked
# (a printed code would be a live 2FA credential).
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sign-lib.sh
source "$DIR/sign-lib.sh"

DO_LOGIN=0
[ "${1:-}" = "--login" ] && DO_LOGIN=1

fail=0
ok()   { printf '  \033[32mOK\033[0m    %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fail=1; }
warn() { printf '  \033[33mWARN\033[0m  %s\n' "$*"; }
sec()  { printf '\n== %s ==\n' "$*"; }

sec "Tools"
for t in python3 xdotool Xvfb fluxbox pkcs11-tool keytool java; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t -> $(command -v "$t")"
    else bad "$t MISSING"; fi
done
for t in osslsigncode import; do
    command -v "$t" >/dev/null 2>&1 && ok "$t (optional) -> $(command -v "$t")" \
        || warn "$t missing — $([ "$t" = import ] && echo 'no failure screenshots (apt install imagemagick)' || echo 'signatures cannot be verified locally')"
done
[ "$fail" = 1 ] && printf '\n  sudo apt-get install -y %s\n' "$SS_APT_PKGS"

sec "SimplySign Desktop"
if [ -d "$SS_DIST" ]; then
    ok "install dir $SS_DIST"
    [ -x "$SS_DIST/SimplySignDesktop_start" ] && ok "launcher SimplySignDesktop_start" \
        || bad "$SS_DIST/SimplySignDesktop_start missing/not executable"
    if [ -n "$PKCS11" ] && [ -f "$PKCS11" ]; then ok "PKCS#11 module $(basename "$PKCS11")"
    else bad "no SimplySignPKCS*.so under $SS_DIST (set SS_PKCS11)"; fi
    # The GUI is driven by blind clicks calibrated against 2.9.14; a different
    # build moves the fields and the login silently fails.
    v=$(strings "$SS_DIST/SimplySignDesktop" 2>/dev/null | grep -oE '^2\.9\.[0-9]+$' | sort -u | head -1)
    if [ -z "$v" ]; then warn "could not read the SimplySign version"
    elif [ "$v" = "2.9.14" ]; then ok "version $v (the one the click positions are calibrated for)"
    else warn "version $v — the login clicks are calibrated for 2.9.14; if login fails, the dialog layout is the prime suspect"; fi
else
    bad "SimplySign Desktop not installed at $SS_DIST (set SS_DIST) — see docs/WINDOWS-SIGNING.md"
fi

sec "Signer (jsign)"
if [ "${#JSIGN[@]}" -gt 0 ]; then ok "jsign: ${JSIGN[*]}"
    [ -n "${JSIGN_JAR:-}" ] && { [ -f "$JSIGN_JAR" ] && ok "JSIGN_JAR exists" || bad "JSIGN_JAR=$JSIGN_JAR does not exist"; }
else bad "jsign not found — set JSIGN_JAR=~/.local/share/jsign.jar or put jsign on PATH"; fi

sec "Credentials"
if [ -n "${CERTUM_EMAIL:-}" ]; then
    local_part="${CERTUM_EMAIL%%@*}"
    ok "CERTUM_EMAIL set (${local_part:0:1}***@${CERTUM_EMAIL#*@})"
else
    bad "CERTUM_EMAIL unset — the Makefile then skips signing entirely (opt-in by design)"
fi
if [ -n "${CERTUM_OTP_URI:-}" ]; then
    ok "CERTUM_OTP_URI set (inline)"
elif [ -n "${CERTUM_OTP_URI_FILE:-}" ]; then
    if [ -f "$CERTUM_OTP_URI_FILE" ]; then
        ok "CERTUM_OTP_URI_FILE -> $CERTUM_OTP_URI_FILE"
        grep -q 'otpauth://' "$CERTUM_OTP_URI_FILE" 2>/dev/null \
            && ok "file contains an otpauth:// URI" \
            || bad "file has no otpauth:// URI — it must hold the TOTP SEED, not a password"
    else
        bad "CERTUM_OTP_URI_FILE=$CERTUM_OTP_URI_FILE does not exist"
    fi
else
    bad "neither CERTUM_OTP_URI nor CERTUM_OTP_URI_FILE is set"
fi
if [ -n "${CERTUM_OTP_URI:-}${CERTUM_OTP_URI_FILE:-}" ] && command -v python3 >/dev/null 2>&1; then
    t=$(ss_totp 2>/dev/null)
    case "$t" in
        ''|*[!0-9]*) bad "TOTP generation failed — the seed does not parse" ;;
        *) ok "TOTP generates (${#t} digits; code not shown)" ;;
    esac
fi

sec "Clock (TOTP is time-based — skew > 30 s rejects every code)"
if command -v timedatectl >/dev/null 2>&1; then
    s=$(timedatectl show -p NTPSynchronized --value 2>/dev/null)
    [ "$s" = "yes" ] && ok "system clock NTP-synchronised" || bad "system clock NOT NTP-synchronised (sudo timedatectl set-ntp true)"
    printf '        now: %s\n' "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
else
    warn "timedatectl unavailable — verify the clock manually ($(date -u '+%H:%M:%S UTC'))"
fi

sec "Environment"
[ -n "${USER:-}" ] && ok "USER=$USER (must be non-empty: the bundled OpenSSL NULL-derefs otherwise)" \
                   || bad "USER is empty — the SimplySign module will SIGSEGV"
lock="/tmp/.X${DISPLAY_NR#:}-lock"
if [ -e "$lock" ]; then
    warn "$lock exists — display $DISPLAY_NR is already in use; Xvfb will refuse to start. Use SS_DISPLAY=:98"
else
    ok "display $DISPLAY_NR is free"
fi
pgrep -f "$SS_DIST/SimplySign" >/dev/null 2>&1 && warn "a SimplySign process is already running (it will be restarted)" \
                                               || ok "no stale SimplySign process"

sec "Token state (is a session already live?)"
if [ -n "$PKCS11" ] && [ -f "$PKCS11" ]; then
    ss_token_live; rc=$?
    case $rc in
        0) ok "token present — a SimplySign session is live right now" ;;
        1) printf '  ----  no token (expected when not logged in)\n' ;;
        2) bad "the pkcs11-tool probe could not run: $SS_PROBE_ERR" ;;
    esac
fi

if [ "$DO_LOGIN" = 1 ]; then
    sec "Live login attempt (SS_DEBUG=1 — snapshots to $SS_DIAG_DIR)"
    SS_DEBUG=1 ss_login && { ok "login succeeded"; ss_log "run code-signing/sign-logout.sh when done"; } || fail=1
fi

printf '\n'
if [ "$fail" = 0 ]; then
    echo "No blocking problems found."
    [ "$DO_LOGIN" = 1 ] || echo "Nothing above touched Certum — add --login to test the real cloud login."
else
    echo "Blocking problems found (FAIL above). See docs/WINDOWS-SIGNING.md."
fi
exit "$fail"
