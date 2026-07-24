#!/bin/bash
# sign-lib.sh — Certum SimplySign Authenticode signing primitives for Mercury's
# Windows binaries, run headlessly on Linux (sourced, not executed).
#
# WHY THIS EXISTS / KEY FACTS (learned the hard way — see docs/WINDOWS-SIGNING.md):
#   * SimplySign has NO headless login: the cloud session is opened by logging
#     into the SimplySign Desktop GUI.  We drive that GUI on a virtual X server
#     (Xvfb + fluxbox + xdotool), typing the account e-mail + a TOTP derived
#     from the otpauth:// URI.  This mirrors reactiveui/actions-common.
#   * The signing tool is jsign (Java), NOT osslsigncode: the SimplySign PKCS#11
#     module does not expose its objects to OpenSC's C_FindObjects
#     (pkcs11-tool -O / osslsigncode return an EMPTY object list), but Java's
#     SunPKCS11 provider enumerates the key by alias.  jsign therefore works
#     where osslsigncode cannot even find the key.  osslsigncode is used only to
#     VERIFY (nice human-readable summary).
#   * The SimplySign module bundles an ancient OpenSSL that NULL-derefs (SIGSEGV,
#     exit 139) when $USER is empty — so USER must be set for EVERY process that
#     loads the module.  A non-login shell / CI runner often has it unset.
#   * The authenticated session and the per-file signing are SEPARATE lifecycles
#     (ss_login is idempotent + persistent; ss_sign_file only signs; ss_logout
#     tears down).  Sign N binaries with ONE login; nothing in the signing path
#     can race a cleanup that kills the daemon mid cloud round-trip.
#
# REQUIRED env (secrets — never committed; keep them OUTSIDE the repo):
#   CERTUM_EMAIL          SimplySign account e-mail
#   CERTUM_OTP_URI_FILE   path to a file containing the otpauth:// URI (TOTP seed)
#     (or CERTUM_OTP_URI   the otpauth:// URI itself)
# Optional env (have sensible defaults):
#   SS_DIST      SimplySign Desktop dir            (/opt/SimplySignDesktop)
#   SS_PKCS11    PKCS#11 module .so                (auto-found under SS_DIST)
#   JSIGN_JAR    path to jsign CLI jar             (else `jsign` on PATH)
#   CERTUM_TSA   RFC-3161 timestamp URL            (http://time.certum.pl)
#   SS_DISPLAY   Xvfb display                      (:99)
#   SS_ALG       digest algorithm                  (SHA-256)

# CRITICAL: bundled-OpenSSL NULL-deref guard (see above).
export USER="${USER:-$(id -un)}"

SS_DIST="${SS_DIST:-/opt/SimplySignDesktop}"
PKCS11="${SS_PKCS11:-$(find "$SS_DIST" -maxdepth 1 -iname 'SimplySignPKCS*.so' 2>/dev/null | head -1)}"
CERTUM_TSA="${CERTUM_TSA:-http://time.certum.pl}"
DISPLAY_NR="${SS_DISPLAY:-:99}"
SS_ALG="${SS_ALG:-SHA-256}"
SS_CONF="${SS_PKCS11_CONF:-/tmp/mercury-sunpkcs11.conf}"

# jsign locator: explicit jar, else a `jsign` launcher on PATH.
if [ -n "${JSIGN_JAR:-}" ]; then JSIGN=(java -jar "$JSIGN_JAR")
elif command -v jsign >/dev/null 2>&1;   then JSIGN=(jsign)
else JSIGN=(); fi

ss_log() { echo "[ssign] $*" >&2; }

ss_require() {
    [ -n "$PKCS11" ] && [ -f "$PKCS11" ] || { ss_log "ERROR: SimplySign PKCS#11 module not found under $SS_DIST (set SS_PKCS11)"; return 1; }
    [ "${#JSIGN[@]}" -gt 0 ] || { ss_log "ERROR: jsign not found — set JSIGN_JAR or put jsign on PATH (see docs/WINDOWS-SIGNING.md)"; return 1; }
    [ -n "${CERTUM_EMAIL:-}" ] || { ss_log "ERROR: CERTUM_EMAIL is unset"; return 1; }
    if [ -z "${CERTUM_OTP_URI:-}" ]; then
        [ -n "${CERTUM_OTP_URI_FILE:-}" ] && [ -f "$CERTUM_OTP_URI_FILE" ] \
            || { ss_log "ERROR: set CERTUM_OTP_URI or CERTUM_OTP_URI_FILE (the otpauth:// TOTP seed)"; return 1; }
    fi
    return 0
}

# --- fresh TOTP from the otpauth:// URI (algorithm/digits/period aware) ---
ss_totp() {
    local uri="${CERTUM_OTP_URI:-$(cat "$CERTUM_OTP_URI_FILE")}"
    python3 - "$uri" <<'PY'
import sys,hmac,hashlib,base64,struct,time
from urllib.parse import urlparse,parse_qs
q=parse_qs(urlparse(sys.argv[1].strip()).query)
s=q['secret'][0]; d=int(q.get('digits',['6'])[0]); p=int(q.get('period',['30'])[0])
a={'SHA1':hashlib.sha1,'SHA256':hashlib.sha256,'SHA512':hashlib.sha512}[q.get('algorithm',['SHA1'])[0].upper()]
k=base64.b32decode(s+'='*((8-len(s)%8)%8))
h=hmac.new(k,struct.pack('>Q',int(time.time())//p),a).digest(); o=h[-1]&0xF
print(str((int.from_bytes(h[o:o+4],'big')&0x7fffffff)%(10**d)).zfill(d))
PY
}

# --- is the token slot present (session live)?  Check -L, not -O. ---
ss_token_live() {
    timeout 15 pkcs11-tool --module "$PKCS11" -L 2>/dev/null | grep -qi 'token label'
}

# --- write the SunPKCS11 config jsign/keytool use to reach the module ---
ss_write_conf() { printf 'name = SimplySign\nlibrary = %s\nslotListIndex = 0\n' "$PKCS11" > "$SS_CONF"; }

# --- the signing key's alias (via SunPKCS11 — OpenSC can't enumerate it) ---
ss_key_alias() {
    ss_write_conf
    timeout 40 keytool -list -keystore NONE -storetype PKCS11 \
        -providerClass sun.security.pkcs11.SunPKCS11 -providerArg "$SS_CONF" \
        -storepass "" 2>/dev/null \
        | awk -F, '/PrivateKeyEntry/{gsub(/ /,"",$1); print $1; exit}'
}

# --- largest on-screen SimplySign window: sets WID BX BY BW BH ---
ss_find_window() {
    WID=""; local best=0
    for w in $(DISPLAY=$DISPLAY_NR xdotool search --name '' 2>/dev/null); do
        local nm; nm=$(DISPLAY=$DISPLAY_NR xdotool getwindowname "$w" 2>/dev/null || true)
        case "$nm" in *implySign*) ;; *) continue ;; esac
        eval "$(DISPLAY=$DISPLAY_NR xdotool getwindowgeometry --shell "$w" 2>/dev/null)"
        local area=$(( ${WIDTH:-0} * ${HEIGHT:-0} ))
        if [ "$area" -gt "$best" ]; then best=$area; WID=$w; BX=${X:-0}; BY=${Y:-0}; BW=${WIDTH:-0}; BH=${HEIGHT:-0}; fi
    done
    [ -n "$WID" ]
}

# --- bring up (or reuse) an authenticated session. Idempotent. No teardown. ---
ss_login() {
    ss_require || return 1
    if ss_token_live; then ss_log "session already live — reusing"; return 0; fi

    ss_log "starting headless SimplySign session on $DISPLAY_NR"
    pkill -f SimplySignDesktop 2>/dev/null || true
    pkill -f "Xvfb $DISPLAY_NR" 2>/dev/null || true
    pkill -f fluxbox 2>/dev/null || true
    sleep 1; rm -f "$HOME/SimplySignDesktop-Lock"

    Xvfb $DISPLAY_NR -screen 0 1280x1024x24 -ac +extension GLX +render &>/dev/null &
    sleep 2
    mkdir -p ~/.fluxbox; echo 'session.screen0.rootCommand: /bin/true' > ~/.fluxbox/init
    DISPLAY=$DISPLAY_NR fluxbox &>/dev/null & sleep 2
    cp "$SS_DIST/SimplySignDesktop.xml" ~/ 2>/dev/null || true
    mkdir -p ~/.config
    printf '[General]\nCacheUserIdAtLogon=Yes\nShowLogonDialogAfterApplicationStartup=Yes\nShowLogonDialogWhenAnyAppRequestsAccess=Yes\n' > ~/.config/"Unknown Organization.conf"
    DISPLAY=$DISPLAY_NR LD_LIBRARY_PATH=$SS_DIST QT_QPA_PLATFORM_PLUGIN_PATH=$SS_DIST/plugins \
        "$SS_DIST/SimplySignDesktop_start" &>/dev/null &

    ss_log "waiting for login window..."
    local ok=""
    for _ in $(seq 1 60); do
        if ss_find_window && [ "${BW:-0}" -ge 300 ] && [ "${BH:-0}" -ge 200 ]; then ok=1; break; fi
        sleep 2
    done
    [ -z "$ok" ] && { ss_log "ERROR: login window did not appear"; return 1; }

    local totp; totp=$(ss_totp)
    DISPLAY=$DISPLAY_NR xdotool windowactivate --sync "$WID" 2>/dev/null || true
    DISPLAY=$DISPLAY_NR xdotool windowraise "$WID" 2>/dev/null || true; sleep 1
    DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*39/100)) click 1; sleep 1
    DISPLAY=$DISPLAY_NR xdotool key --clearmodifiers ctrl+a
    DISPLAY=$DISPLAY_NR xdotool type --clearmodifiers --delay 50 "$CERTUM_EMAIL"; sleep 1
    DISPLAY=$DISPLAY_NR xdotool key Tab; sleep 1
    DISPLAY=$DISPLAY_NR xdotool type --clearmodifiers --delay 50 "$totp"; sleep 1
    DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*76/100)) click 1; sleep 8
    # dismiss the "Logon successful" dialog (Close button, bottom-centre)
    if ss_find_window; then
        DISPLAY=$DISPLAY_NR xdotool windowactivate --sync "$WID" 2>/dev/null || true
        DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*94/100)) click 1
    fi

    ss_log "waiting for PKCS#11 token to come online..."
    for _ in $(seq 1 30); do ss_token_live && { ss_log "session live"; return 0; }; sleep 2; done
    ss_log "ERROR: token did not come online (login likely failed)"; return 1
}

# --- sign ONE file IN PLACE (jsign adds the Authenticode signature); retries --
ss_sign_file() {
    local in="$1"
    [ -f "$in" ] || { ss_log "ERROR: no such file: $in"; return 1; }
    ss_login || return 1
    local alias; alias=$(ss_key_alias)
    [ -z "$alias" ] && { ss_log "ERROR: no signing key visible via SunPKCS11"; return 1; }

    local attempt rc=1
    for attempt in 1 2 3; do
        ss_log "signing $in (alias $alias, attempt $attempt)"
        if "${JSIGN[@]}" --storetype PKCS11 --keystore "$SS_CONF" --storepass "" \
                --alias "$alias" --alg "$SS_ALG" --tsaurl "$CERTUM_TSA" "$in" \
                2>&1 | grep -viE '^Warning|proprietary|will be removed' | sed 's/^/[jsign] /' >&2; then
            rc=0; break
        fi
        ss_log "attempt $attempt failed (transient cloud/timestamp?) — retrying"; sleep 5
    done
    if [ "$rc" = 0 ]; then
        ss_log "OK: signed $in"
        command -v osslsigncode >/dev/null && \
            osslsigncode verify "$in" 2>&1 | grep -iE 'Subject:|Issuer :|Serial :|Timestamp' | sed 's/^/[verify] /' >&2 || true
    else
        ss_log "ERROR: failed to sign $in after 3 attempts"
    fi
    return $rc
}

# --- tear the session down ---
ss_logout() {
    ss_log "tearing down SimplySign session"
    pkill -f SimplySignDesktop 2>/dev/null || true
    pkill -f "Xvfb $DISPLAY_NR" 2>/dev/null || true
    pkill -f fluxbox 2>/dev/null || true
    rm -f "$HOME/SimplySignDesktop-Lock"
}
