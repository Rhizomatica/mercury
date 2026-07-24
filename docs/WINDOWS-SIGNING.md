# Windows code signing (Authenticode)

Mercury's Windows binaries and installer can be Authenticode-signed. Three
approaches:

| Mode | Tool | When |
|---|---|---|
| **A) SimplySign cloud** | `code-signing/sign.sh` (Xvfb + xdotool login; **jsign** over PKCS#11) | **Production path** — the real Certum cert, no `.pfx` file |
| **B) Local .pfx** | `osslsigncode -pkcs12` | Self-signed test cert only (see below) |
| **C) Inno Setup** | `signtool.exe` on Windows | Signing the installer (Windows VM / physical machine) |

**Mode A (SimplySign cloud) is the production path.** The certificate is a
*Certum Open Source Developer* cert whose private key lives in Certum's cloud
HSM — there is no `.pfx` to export. The in-repo scripts under `code-signing/`
open a SimplySign session by automating the SimplySign Desktop GUI on a virtual
X server, then sign with **jsign**. See `code-signing/README.md` for the
quickstart; this document is the full reference.

### Three facts that make (or break) this — learned the hard way

1. **The signer is `jsign` (Java), not `osslsigncode`.** This SimplySign PKCS#11
   module (`SimplySignPKCS_64-MS-1.0.20.so`, shipped with SimplySign Desktop
   2.9.14) does **not** expose its objects to OpenSC's `C_FindObjects`:
   `pkcs11-tool -O`, `p11tool`, and therefore `osslsigncode -pkcs11module`
   all see an **empty** object list, so osslsigncode cannot even find the key
   (and crashes if pushed via the OpenSSL `pkcs11` engine — see below). Java's
   **SunPKCS11** provider *does* enumerate the key by alias, so `jsign`
   (and `keytool`) work where osslsigncode cannot. osslsigncode is kept **only
   to verify** the result (nice human-readable summary). This matches the
   upstream reactiveui `certum-sign` action, which also uses jsign.
2. **`USER` must be set.** The SimplySign module bundles an ancient OpenSSL that
   NULL-derefs (SIGSEGV, exit 139) when `$USER` is empty — so **every** process
   that loads the module (SimplySign Desktop, `keytool`, `jsign`) needs `USER`
   set. Non-login shells and CI runners often have it empty. `sign-lib.sh`
   exports it (`export USER="${USER:-$(id -un)}"`).
3. **Login once, sign many.** The authenticated session and the per-file signing
   are separate lifecycles: `ss_login` is idempotent + persistent, `ss_sign_file`
   only signs, `ss_logout` tears down. A slow cloud round-trip therefore can
   never be killed by a cleanup — the failure the old monolithic script hit.

> Do **not** install `libengine-pkcs11-openssl` / use the OpenSSL `pkcs11`
> engine for this cert — loading the OpenSSL-1.0.0-based module into an
> OpenSSL-3.x process segfaults. jsign (separate JVM) avoids the ABI clash.

## Secrets — where they live (NEVER in the repo)

Signing needs two account secrets. **Keep them OUTSIDE the git tree** and point
env vars at them. `.gitignore` also blocks `*otpauth* pass.txt *.pfx *.p12
*sunpkcs11*.conf jsign*.jar` as a backstop, but the rule is: secrets live in
your home, not in the repo.

| Secret | What it is | Suggested location (mode 600) | Env var |
|---|---|---|---|
| otpauth URI | TOTP seed `otpauth://totp/...?secret=...` — 2FA-equivalent | `~/.config/mercury-signing/otpauth.txt` | `CERTUM_OTP_URI_FILE` (or `CERTUM_OTP_URI`) |
| account e-mail | SimplySign login e-mail | shell env | `CERTUM_EMAIL` |

The public certificate, the PKCS#11 module and the SimplySign Desktop install
are **not** secret. The maintainer's copies live under
`~/files/documents/windows_certum/` (outside the repo).

## Prerequisites

```bash
# headless X + PKCS#11 + verify + Java (for jsign)
sudo apt-get install -y xvfb fluxbox xdotool opensc osslsigncode default-jre-headless

# jsign — the signer (single jar); point JSIGN_JAR at it (or put `jsign` on PATH)
curl -fsSLo ~/.local/share/jsign.jar \
  https://repo1.maven.org/maven2/net/jsign/jsign/7.1/jsign-7.1.jar
```

SimplySign Desktop must be installed at `/opt/SimplySignDesktop` (Certum's
bundle; ships its own Qt 5.9.2 + OpenSSL 1.0.0 — no system Qt/OpenSSL needed).
It runs fine on Xvfb (software GL); on a real GPU display its old Qt hangs.

## Mode A: SimplySign cloud signing (production)

```bash
export CERTUM_EMAIL="you@example.org"
export CERTUM_OTP_URI_FILE="$HOME/.config/mercury-signing/otpauth.txt"
export JSIGN_JAR="$HOME/.local/share/jsign.jar"

make sign-windows                       # sign mercury.exe
make sign-windows-bin BIN=mercury-ui.exe
make windows-zip-signed                 # build + sign both + zip (one login)
```

The Makefile calls `code-signing/sign.sh`, which:
1. Starts Xvfb `:99` + fluxbox (virtual display) with `USER` set
2. Launches SimplySign Desktop, waits for the login window
3. Generates a fresh TOTP from the otpauth URI, types e-mail + TOTP, clicks Login
4. Dismisses the "Logon successful" dialog; waits for the PKCS#11 token
5. Signs each file **in place** with `jsign` via SunPKCS11 (alias auto-detected)
6. Verifies with `osslsigncode`
7. `sign-logout.sh` tears the session down at the end of `windows-zip-signed`

Signing is **opt-in**: `make windows-zip` (and the whole flow) stays *unsigned*
unless `CERTUM_EMAIL` is set in the environment, so ordinary builds never touch
the cloud.

## CI

A ready-to-adapt GitHub Actions workflow is in
`code-signing/.github-workflow-example/sign-windows.yml`. It runs inside
reactiveui's prebuilt `ghcr.io/reactiveui/certum-signer` image (SimplySign +
jsign + X11 already installed) and reads `CERTUM_EMAIL` / `CERTUM_OTP_URI` from
GitHub Actions secrets. Move it to `.github/workflows/` and add the two secrets.

## What can be signed, and where

| Artifact | Built on | Signed on | Tool |
|---|---|---|---|
| `mercury.exe`, `mercury-ui.exe` | Linux (mingw) | Linux | `osslsigncode` via PKCS#11 (`make sign-windows`) |
| `Mercury_*_Setup.exe` (installer) | Windows or Wine (Inno Setup ISCC) | Linux | `osslsigncode` via PKCS#11 (`make sign-windows-bin BIN=Setup.exe`) |

Everything is signed on Linux with `osslsigncode` + PKCS#11. The Inno Setup
compiler (ISCC) builds the installer unsigned — then the resulting `.exe` is
signed afterward, exactly like the payload binaries. The Inno `SignTool`
directive is not used; post-build signing is simpler and works from CI.>

## Setup: SimplySign Desktop + signing scripts

The SimplySign Desktop binary is bundled at `/opt/SimplySignDesktop`. The
signing scripts live in `~/files/MYSELF/code-signing/`:

| File | Purpose |
|---|---|
| `sign.sh` | Main entry — Xvfb + xdotool + TOTP automation + `osslsigncode` |
| `otpauthuri.txt` → `~/files/documents/windows_certum/otpauthuri.txt` | TOTP secret seed |
| `219d71faf992d043051392dc77eb705b.pem` | Public certificate (in `~/files/documents/windows_certum/`) |
| `SIGNING.md` | Full reference doc (in `~/files/MYSELF/code-signing/`) |

## Parameters reference (Makefile)

| Variable | Default | Meaning |
|---|---|---|
| `WIN_SIGN_PFX` | *(empty)* | Path to a local PKCS#12 (`.pfx`) cert+key. **When set**: uses `osslsigncode -pkcs12`. **When empty**: uses SimplySign cloud via `sign.sh` (default). |
| `WIN_SIGN_PASS` | *(empty)* | Password for the `.pfx` (only used when `WIN_SIGN_PFX` is set). |
| `WIN_SIGN_TS` | `http://timestamp.digicert.com` | RFC-3161 timestamp authority URL. Certum's timestamp server: `http://time.certum.pl` |
| `WIN_SIGN_NAME` | `Mercury HF Modem` | Signature description (`-n`). |
| `WIN_SIGN_URL` | `https://github.com/Rhizomatica/mercury` | Signature info URL (`-i`). |
| `SIGN_SCRIPT` | `$(HOME)/files/MYSELF/code-signing/sign.sh` | Path to the SimplySign automation script. |

## Mode B: Local .pfx (self-signed test or OV/EV token)

Set `WIN_SIGN_PFX` and `WIN_SIGN_PASS` to use a local PKCS#12 file:

```bash
make windows-zip \
     WIN_SIGN_PFX=$PWD/windows-installer/mercury-selfsign.pfx \
     WIN_SIGN_PASS=mercury
```

To generate a self-signed test certificate:
```bash
cd windows-installer
./gen-selfsigned-cert.sh    # -> mercury-selfsign.pfx (password: mercury)
```

Manual equivalent:
```bash
osslsigncode sign -pkcs12 mercury-selfsign.pfx -pass mercury \
    -n "Mercury HF Modem" -i https://github.com/Rhizomatica/mercury \
    -h sha256 -ts http://timestamp.digicert.com \
    -in mercury.exe -out mercury-signed.exe
osslsigncode verify mercury-signed.exe
```

`.pfx` / `.p12` files are git-ignored — never commit a signing key.

## Mode C: Sign the installer

The Inno Setup compiler (ISCC) builds `Mercury_$(VERSION)_Setup.exe`. That `.exe`
is then signed with the same `osslsigncode` PKCS#11 path as the payload binaries:

```bash
# On Linux (or Windows with Inno):
ISCC windows-installer/installer.iss          # builds Mercury_1.9.10_Setup.exe

# Then sign it on Linux:
make sign-windows-bin BIN=Mercury_1.9.10_Setup.exe
```

If you prefer Inno-side signing on Windows (with SimplySign Desktop installed):
```bat
ISCC /DSIGN /Smercury="signtool sign /a /fd sha256 /tr http://time.certum.pl /td sha256 $f" installer.iss
```

The `/a` flag auto-selects the cert from the Windows cert store. But the
recommended path is post-build signing on Linux — same tooling, same CI.

## Certificate details

| Field | Value |
|---|---|
| Type | Open Source Developer (Unqualified) |
| Subject | Open Source Developer Rafael Diniz |
| Issuer | Certum Code Signing 2021 CA |
| Serial | `219d71faf992d0000000000000000000` |
| Valid | 2026-07-24 to 2027-07-24 |
| Account | `rafael@rhizomatica.org` |
| Timestamp URL | `http://time.certum.pl` |
