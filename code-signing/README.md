# Mercury Windows code signing (Certum SimplySign, headless Linux)

Authenticode-sign the Windows binaries on Linux using a **Certum SimplySign**
cloud certificate — no `.pfx` file, no Windows VM. Full background,
troubleshooting and CI in [`../docs/WINDOWS-SIGNING.md`](../docs/WINDOWS-SIGNING.md).

```
sign-lib.sh      signing primitives (sourced): login-once, sign, logout
sign.sh          sign one or more .exe IN PLACE (reuses one login)
sign-logout.sh   tear the SimplySign session down
sign-diag.sh     "why won't this work here?" — environment report, no cloud call
```

**Signing failed? Run `./sign-diag.sh` first.** The login is a blind GUI drive,
so a missing package, a skewed clock, a busy display and a genuinely rejected
password all used to surface as the same unhelpful `token did not come online`.
`sign-diag.sh` checks each separately and names the one that is wrong; add
`--login` to attempt the real login with a screenshot of every step. See the
Troubleshooting section of [`../docs/WINDOWS-SIGNING.md`](../docs/WINDOWS-SIGNING.md).

## Secrets — where they go (NEVER in the repo)

The certificate's private key lives in Certum's cloud HSM; signing needs two
account secrets. **Keep them OUTSIDE the git tree** and point env vars at them.
The repo's `.gitignore` also blocks `*.pfx *.p12 *.otpauth *otpauth* pass.txt`
as a backstop, but the real rule is: secrets live in your home, not here.

| Secret | What it is | Suggested location | Env var |
|---|---|---|---|
| otpauth URI | TOTP seed (`otpauth://totp/...?secret=...`) — 2FA equivalent, **guard it** | `~/.config/mercury-signing/otpauth.txt` (mode 600) | `CERTUM_OTP_URI_FILE` (or `CERTUM_OTP_URI`) |
| account e-mail | SimplySign login e-mail | env / your shell rc | `CERTUM_EMAIL` |

Nothing else is secret: the public certificate, the PKCS#11 module and the
SimplySign Desktop install are not sensitive.

Get the otpauth URI once, when enrolling the SimplySign mobile token: Certum
shows a QR code — decode it to the `otpauth://` URI and save that text to the
file above. (The maintainer's copy lives at
`~/files/documents/windows_certum/otpauthuri.txt`, outside the repo.)

## One-time setup

```bash
# packages (Debian/Ubuntu): headless X + PKCS#11 + verify + Java for jsign
sudo apt-get install -y xvfb fluxbox xdotool opensc osslsigncode default-jre-headless

# jsign (the signer — see docs for WHY not osslsigncode): one jar
curl -fsSLo ~/.local/share/jsign.jar \
  https://repo1.maven.org/maven2/net/jsign/jsign/7.1/jsign-7.1.jar

# SimplySign Desktop (Certum's bundle) -> /opt/SimplySignDesktop  (see docs)
```

## Sign

```bash
export CERTUM_EMAIL="you@example.org"
export CERTUM_OTP_URI_FILE="$HOME/.config/mercury-signing/otpauth.txt"
export JSIGN_JAR="$HOME/.local/share/jsign.jar"

# via the Makefile (recommended — one login for the whole release):
make windows-zip-signed

# or directly:
code-signing/sign.sh mercury.exe mercury-ui.exe
code-signing/sign-logout.sh          # when done
```

## The three things that make this work (details in the doc)

1. **jsign, not osslsigncode.** This SimplySign PKCS#11 module does not expose
   its objects to OpenSC (`pkcs11-tool -O` / osslsigncode return an empty list),
   but Java's SunPKCS11 enumerates the key by alias — so jsign signs where
   osslsigncode cannot even find the key. osslsigncode is used only to *verify*.
2. **`USER` must be set.** The module bundles an ancient OpenSSL that segfaults
   (exit 139) when `$USER` is empty; `sign-lib.sh` exports it.
3. **Login once, sign many.** The session and the per-file signing are separate
   lifecycles, so a slow cloud round-trip can't be killed by a cleanup.
