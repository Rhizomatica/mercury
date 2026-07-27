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
#   * The login is a BLIND GUI drive (clicks at fixed fractions of the dialog).
#     So every "it didn't work" is really "the screen showed something we did
#     not expect".  ss_snap() therefore captures a screenshot + window inventory
#     on every failure — without a picture the operator only ever saw the
#     useless "token did not come online".  Run with SS_DEBUG=1 to snapshot
#     every step.
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
#   SS_DEBUG     1 = snapshot every login step     (off)
#   SS_DIAG_DIR  where snapshots/logs land         (/tmp/mercury-signing-diag)

# CRITICAL: bundled-OpenSSL NULL-deref guard (see above).
export USER="${USER:-$(id -un)}"

SS_DIST="${SS_DIST:-/opt/SimplySignDesktop}"
PKCS11="${SS_PKCS11:-$(find "$SS_DIST" -maxdepth 1 -iname 'SimplySignPKCS*.so' 2>/dev/null | head -1)}"
CERTUM_TSA="${CERTUM_TSA:-http://time.certum.pl}"
DISPLAY_NR="${SS_DISPLAY:-:99}"
SS_ALG="${SS_ALG:-SHA-256}"
SS_CONF="${SS_PKCS11_CONF:-/tmp/mercury-sunpkcs11.conf}"
SS_DIAG_DIR="${SS_DIAG_DIR:-/tmp/mercury-signing-diag}"
# Where the login records the PIDs it started, so ss_logout kills OUR Xvfb and
# OUR fluxbox and nothing else.  (A blind `pkill -f fluxbox` would take out the
# operator's own desktop if they happen to run fluxbox.)
SS_RUN_DIR="${SS_RUN_DIR:-${XDG_RUNTIME_DIR:-/tmp}/mercury-signing}"

# jsign locator: explicit jar, else a `jsign` launcher on PATH.
if [ -n "${JSIGN_JAR:-}" ]; then JSIGN=(java -jar "$JSIGN_JAR")
elif command -v jsign >/dev/null 2>&1;   then JSIGN=(jsign)
else JSIGN=(); fi

ss_log() { echo "[ssign] $*" >&2; }
ss_have() { command -v "$1" >/dev/null 2>&1; }

# Everything the login/sign path shells out to.  Checking UP FRONT matters: a
# missing python3 (-> empty TOTP typed) and a missing pkcs11-tool (-> the
# liveness probe can never succeed) both used to surface minutes later as the
# same misleading "token did not come online (login likely failed)".
SS_APT_PKGS="xvfb fluxbox xdotool opensc osslsigncode default-jre-headless imagemagick python3"

ss_require_tools() {
    local missing="" t
    for t in python3 xdotool Xvfb fluxbox pkcs11-tool keytool; do
        ss_have "$t" || missing="$missing $t"
    done
    [ -z "$missing" ] || {
        ss_log "ERROR: missing required tool(s):$missing"
        ss_log "       sudo apt-get install -y $SS_APT_PKGS"
        return 1
    }
    ss_have import || ss_have xwd || \
        ss_log "note: no screenshot tool (apt install imagemagick) — failures will show the window list only"
    return 0
}

ss_require() {
    ss_require_tools || return 1
    [ -d "$SS_DIST" ] || { ss_log "ERROR: SimplySign Desktop not installed at $SS_DIST (set SS_DIST)"; return 1; }
    [ -n "$PKCS11" ] && [ -f "$PKCS11" ] || { ss_log "ERROR: SimplySign PKCS#11 module not found under $SS_DIST (set SS_PKCS11)"; return 1; }
    [ -x "$SS_DIST/SimplySignDesktop_start" ] || { ss_log "ERROR: $SS_DIST/SimplySignDesktop_start missing or not executable"; return 1; }
    [ "${#JSIGN[@]}" -gt 0 ] || { ss_log "ERROR: jsign not found — set JSIGN_JAR or put jsign on PATH (see docs/WINDOWS-SIGNING.md)"; return 1; }
    [ -n "${CERTUM_EMAIL:-}" ] || { ss_log "ERROR: CERTUM_EMAIL is unset"; return 1; }
    if [ -z "${CERTUM_OTP_URI:-}" ]; then
        [ -n "${CERTUM_OTP_URI_FILE:-}" ] && [ -f "$CERTUM_OTP_URI_FILE" ] \
            || { ss_log "ERROR: set CERTUM_OTP_URI or CERTUM_OTP_URI_FILE (the otpauth:// TOTP seed)"; return 1; }
        grep -q 'otpauth://' "$CERTUM_OTP_URI_FILE" 2>/dev/null \
            || { ss_log "ERROR: $CERTUM_OTP_URI_FILE does not contain an otpauth:// URI (it holds the TOTP seed, not a password)"; return 1; }
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
#     0 = live, 1 = no token yet, 2 = the PROBE ITSELF could not run
#     (pkcs11-tool absent, hung, or SIGSEGVing on an empty $USER).  Telling 2
#     apart from 1 matters: 2 is not a failed login at all, and used to be
#     reported as one.
SS_PROBE_ERR=""
ss_token_live() {
    local out rc
    out=$(timeout 15 pkcs11-tool --module "$PKCS11" -L 2>&1); rc=$?
    printf '%s' "$out" | grep -qi 'token label' && { SS_PROBE_ERR=""; return 0; }
    # 124 = timeout(1) kill, 127 = command not found, >128 = killed by a signal
    if [ "$rc" -ge 124 ]; then
        SS_PROBE_ERR="pkcs11-tool exited $rc: $(printf '%s' "$out" | head -2 | tr '\n' ' ')"
        return 2
    fi
    SS_PROBE_ERR=""
    return 1
}

# --- capture what the virtual screen actually shows ---
# The blind GUI drive means a failure is almost always "an unexpected window is
# up" (error popup, a SimplySign build whose dialog moved, a first-run wizard).
# A picture turns a 30-minute guess into a 10-second read.
ss_snap() {
    local tag="${1:-snap}" stamp png w nm geo
    mkdir -p "$SS_DIAG_DIR" 2>/dev/null || return 0
    stamp="$(date +%H%M%S)"
    png="$SS_DIAG_DIR/$stamp-$tag.png"
    if ss_have import; then
        DISPLAY=$DISPLAY_NR import -window root "$png" 2>/dev/null && ss_log "diag: screenshot $png"
    elif ss_have xwd; then
        DISPLAY=$DISPLAY_NR xwd -root -silent > "${png%.png}.xwd" 2>/dev/null && \
            ss_log "diag: screenshot ${png%.png}.xwd (apt install imagemagick for PNG)"
    fi
    {
        echo "--- windows on $DISPLAY_NR ($tag, $stamp) ---"
        for w in $(DISPLAY=$DISPLAY_NR xdotool search --name '' 2>/dev/null); do
            nm=$(DISPLAY=$DISPLAY_NR xdotool getwindowname "$w" 2>/dev/null)
            [ -n "$nm" ] || continue
            geo=$(DISPLAY=$DISPLAY_NR xdotool getwindowgeometry "$w" 2>/dev/null | tr '\n' ' ')
            echo "  [$w] '$nm' $geo"
        done
    } | tee -a "$SS_DIAG_DIR/windows.log" >&2
}

# snapshot only when SS_DEBUG=1 (step-by-step tracing)
ss_snap_debug() { [ "${SS_DEBUG:-0}" = 1 ] && ss_snap "$@"; return 0; }

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
    ss_kill_session
    sleep 1; rm -f "$HOME/SimplySignDesktop-Lock"
    mkdir -p "$SS_RUN_DIR" 2>/dev/null || true

    # Refuse to drive a display we did not create.  The login types the account
    # e-mail and a live TOTP with xdotool; on a display that already has a
    # session those keystrokes land in whatever window happens to be focused —
    # and Xvfb would have silently refused to start there anyway.
    if DISPLAY=$DISPLAY_NR xdotool getdisplaygeometry >/dev/null 2>&1; then
        ss_log "ERROR: display $DISPLAY_NR is already in use by another X server."
        ss_log "       Refusing to type credentials into a session we do not own."
        ss_log "       Pick a free display (SS_DISPLAY=:98) or stop whatever owns $DISPLAY_NR."
        return 1
    fi

    Xvfb $DISPLAY_NR -screen 0 1280x1024x24 -ac +extension GLX +render &>/dev/null &
    local xvfb_pid=$!
    echo "$xvfb_pid" > "$SS_RUN_DIR/xvfb.pid" 2>/dev/null || true
    local i ok=""
    for i in $(seq 1 10); do
        kill -0 "$xvfb_pid" 2>/dev/null || break     # Xvfb died -> stop waiting
        DISPLAY=$DISPLAY_NR xdotool getdisplaygeometry >/dev/null 2>&1 && { ok=1; break; }
        sleep 1
    done
    [ -n "$ok" ] || {
        ss_log "ERROR: Xvfb did not come up on $DISPLAY_NR."
        ss_log "       Stale lock? rm -f /tmp/.X${DISPLAY_NR#:}-lock. Missing? apt-get install -y xvfb."
        return 1
    }

    mkdir -p ~/.fluxbox; echo 'session.screen0.rootCommand: /bin/true' > ~/.fluxbox/init
    DISPLAY=$DISPLAY_NR fluxbox &>/dev/null &
    echo $! > "$SS_RUN_DIR/fluxbox.pid" 2>/dev/null || true
    sleep 2
    cp "$SS_DIST/SimplySignDesktop.xml" ~/ 2>/dev/null || true
    mkdir -p ~/.config
    printf '[General]\nCacheUserIdAtLogon=Yes\nShowLogonDialogAfterApplicationStartup=Yes\nShowLogonDialogWhenAnyAppRequestsAccess=Yes\n' > ~/.config/"Unknown Organization.conf"
    DISPLAY=$DISPLAY_NR LD_LIBRARY_PATH=$SS_DIST QT_QPA_PLATFORM_PLUGIN_PATH=$SS_DIST/plugins \
        "$SS_DIST/SimplySignDesktop_start" &>/dev/null &

    ss_log "waiting for login window..."
    ok=""
    for i in $(seq 1 60); do
        if ss_find_window && [ "${BW:-0}" -ge 300 ] && [ "${BH:-0}" -ge 200 ]; then ok=1; break; fi
        sleep 2
    done
    [ -z "$ok" ] && { ss_log "ERROR: login window did not appear"; ss_snap "no-login-window"; return 1; }
    ss_snap_debug "01-login-window"

    local totp; totp=$(ss_totp)
    case "$totp" in
        ''|*[!0-9]*)
            ss_log "ERROR: could not derive a TOTP from the otpauth:// seed"
            ss_log "       check that ${CERTUM_OTP_URI_FILE:-\$CERTUM_OTP_URI} holds otpauth://totp/...?secret=..."
            return 1 ;;
    esac
    # TOTP is time-based: a skewed clock invalidates every code we type, and the
    # only symptom is a login that silently does not take.
    if ss_have timedatectl && \
       [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "no" ]; then
        ss_log "WARNING: system clock is not NTP-synchronised — TOTP codes will be rejected if it has drifted"
    fi

    DISPLAY=$DISPLAY_NR xdotool windowactivate --sync "$WID" 2>/dev/null || true
    DISPLAY=$DISPLAY_NR xdotool windowraise "$WID" 2>/dev/null || true; sleep 1
    DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*39/100)) click 1; sleep 1
    DISPLAY=$DISPLAY_NR xdotool key --clearmodifiers ctrl+a
    DISPLAY=$DISPLAY_NR xdotool type --clearmodifiers --delay 50 "$CERTUM_EMAIL"; sleep 1
    DISPLAY=$DISPLAY_NR xdotool key Tab; sleep 1
    DISPLAY=$DISPLAY_NR xdotool type --clearmodifiers --delay 50 "$totp"; sleep 1
    ss_snap_debug "02-filled"
    DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*76/100)) click 1; sleep 8
    ss_snap_debug "03-after-login-click"
    # dismiss the "Logon successful" dialog (Close button, bottom-centre)
    if ss_find_window; then
        DISPLAY=$DISPLAY_NR xdotool windowactivate --sync "$WID" 2>/dev/null || true
        DISPLAY=$DISPLAY_NR xdotool mousemove $((BX + BW/2)) $((BY + BH*94/100)) click 1
    fi

    ss_log "waiting for PKCS#11 token to come online..."
    local rc=1
    for i in $(seq 1 30); do
        ss_token_live; rc=$?
        [ "$rc" = 0 ] && { ss_log "session live"; return 0; }
        sleep 2
    done

    # --- failure: say WHICH failure, and show the screen ---
    ss_snap "login-failed"
    if [ "$rc" = 2 ]; then
        ss_log "ERROR: the token probe never ran — ${SS_PROBE_ERR:-pkcs11-tool could not be executed}"
        ss_log "       This is NOT a rejected login: we could not ask whether a token is there."
        ss_log "       Check 'opensc' is installed, and that USER is set (an empty \$USER"
        ss_log "       SIGSEGVs the OpenSSL bundled with the SimplySign module)."
        return 1
    fi
    ss_log "ERROR: the GUI login did not take (no token after 60 s). Screen captured above."
    ss_log "       Usual causes, in order:"
    ss_log "        1. wrong e-mail, wrong/stale otpauth seed, or a clock skew > 30 s"
    ss_log "        2. the account holds no cloud certificate (right password, wrong account)"
    ss_log "        3. a SimplySign Desktop other than 2.9.14: the dialog layout moved, so the"
    ss_log "           blind clicks at 39%/76%/94% of the window hit the wrong widgets"
    ss_log "       Re-run with SS_DEBUG=1 to capture every step into $SS_DIAG_DIR,"
    ss_log "       or run code-signing/sign-diag.sh for a full environment report."
    return 1
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

# --- pattern-kill that can never be a suicide note ---
# `pkill -f` matches COMMAND LINES, so any process that merely *mentions* the
# pattern is a target — including the shell running this very script, or an
# operator's own `pgrep -f /opt/SimplySignDesktop/...`.  We have been bitten by
# exactly this (a teardown that killed its own caller).  Skip our whole ancestor
# chain; pkill already spares itself.
ss_pkill_safe() {
    local pat="$1" pid p ancestors=""
    p=$$
    while [ -n "$p" ] && [ "$p" != 0 ] && [ "$p" != 1 ]; do
        ancestors="$ancestors $p"
        p=$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')
    done
    for pid in $(pgrep -f "$pat" 2>/dev/null); do
        case " $ancestors " in *" $pid "*) continue ;; esac
        kill "$pid" 2>/dev/null
    done
    return 0
}

# --- kill the processes THIS tooling started (never the operator's own) ---
ss_kill_session() {
    local f pid
    for f in xvfb fluxbox; do
        pid=$(cat "$SS_RUN_DIR/$f.pid" 2>/dev/null)
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
        rm -f "$SS_RUN_DIR/$f.pid" 2>/dev/null
    done
    # SimplySign Desktop must still be matched by pattern: the launcher forks the
    # real daemon, so its recorded pid is not the process holding the session.
    # Matching the install path keeps unrelated processes out of scope.
    ss_pkill_safe "$SS_DIST/SimplySign"
    # Fallback for a session started before pid files existed: match Xvfb's own
    # display argument only.  Deliberately NO blind `pkill -f fluxbox` — that
    # would kill the operator's desktop window manager.
    ss_pkill_safe "Xvfb $DISPLAY_NR"
    return 0
}

# --- tear the session down ---
ss_logout() {
    ss_log "tearing down SimplySign session"
    ss_kill_session
    rm -f "$HOME/SimplySignDesktop-Lock"
}
