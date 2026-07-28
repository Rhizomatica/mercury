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
# headless X + PKCS#11 + verify + Java (for jsign) + screenshots (diagnostics)
sudo apt-get install -y xvfb fluxbox xdotool opensc osslsigncode default-jre-headless imagemagick

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

## Troubleshooting

**Check the machine first — it takes two seconds and touches no cloud service:**

```bash
code-signing/sign-diag.sh            # environment report (tools, creds, clock, display)
code-signing/sign-diag.sh --login    # ...and attempt a real login, snapshotting every step
```

It never prints a secret: the e-mail is masked and the TOTP is only
shape-checked (a printed code would be a live 2FA credential).

### `ERROR: token did not come online (login likely failed)`

Step 4 timed out: 60 s after the login click, no PKCS#11 token appeared. The
message names *a* symptom, not the cause — **five** unrelated faults land here,
so do not assume the password is wrong:

| Cause | How to confirm | Fix |
|---|---|---|
| **`opensc` not installed** — `pkcs11-tool` is what asks "is a token there?". Without it the probe can never succeed *even though the login worked*. | `command -v pkcs11-tool` | `sudo apt-get install -y opensc` |
| **Clock skew > 30 s** — the TOTP is time-based, so every code typed is stale. | `timedatectl` → `System clock synchronized: yes` | `sudo timedatectl set-ntp true` |
| **Wrong account / no certificate** — the login succeeds but that account holds no cloud cert, so no token ever appears. | screenshot shows a normal logged-in window | use the account the cert was issued to |
| **Stale or malformed otpauth seed** — the file must hold the `otpauth://totp/...?secret=...` **URI**, not a password. | `sign-diag.sh` → "TOTP generates" | re-export the seed from the SimplySign enrolment QR |
| **SimplySign Desktop ≠ 2.9.14** — the login is a *blind GUI drive*; the clicks land at fixed fractions (39 % / 76 % / 94 %) of the dialog. A different build moves the fields, so the credentials go into the wrong widget. | screenshot shows a dialog with fields still empty / a different layout | install 2.9.14, or re-calibrate the percentages in `ss_login` |

Since the login is blind, **the screenshot is the diagnostic**. Every failure
now writes one, plus a window inventory, to `$SS_DIAG_DIR`
(default `/tmp/mercury-signing-diag`):

```bash
SS_DEBUG=1 make windows-zip-signed     # snapshot every step, not just the failure
ls /tmp/mercury-signing-diag/          # 01-login-window.png, 02-filled.png, ...
```

`02-filled.png` is the one that settles it: if the e-mail and code are sitting
in the right fields, the credentials are being *delivered* and the fault is the
account or the clock; if they are not, it is the GUI layout.

### Other failures

| Symptom | Cause |
|---|---|
| `missing required tool(s): …` | Preflight — install the named packages (the whole list is in Prerequisites). |
| `display :99 is already in use by another X server` | Something else owns `:99`; the login refuses to type credentials into a session it does not own. Use `SS_DISPLAY=:98`. |
| `Xvfb did not come up on :99` | Stale lock: `rm -f /tmp/.X99-lock`. |
| `the token probe never ran` | `pkcs11-tool` itself failed — usually `opensc` missing, or an empty `$USER` (the bundled OpenSSL NULL-derefs; `sign-lib.sh` exports it). |
| `no signing key visible via SunPKCS11` | The session is live but the cert is not on this account — or `keytool`/Java is missing. |
| `WARNING: no signing method available — X is unsigned` | `CERTUM_EMAIL` is unset, so signing was skipped (this is the opt-in default, not an error). |

## CI

A ready-to-adapt GitHub Actions workflow is in
`code-signing/.github-workflow-example/sign-windows.yml`. It runs inside
reactiveui's prebuilt `ghcr.io/reactiveui/certum-signer` image (SimplySign +
jsign + X11 already installed) and reads `CERTUM_EMAIL` / `CERTUM_OTP_URI` from
GitHub Actions secrets. Move it to `.github/workflows/` and add the two secrets.

## What can be signed, and where

| Artifact | Ships in | Built on | Signed by |
|---|---|---|---|
| `mercury.exe` (console) | ZIP | Linux (mingw) | `make windows-zip-signed` |
| `mercury-ui.exe` (GUI, core embedded) | ZIP **and** installer | Linux (mingw) | `make windows-zip-signed` / `make windows-installer-signed` |
| `Mercury_*_Setup.exe` (installer) | — | Windows or Wine (Inno Setup ISCC) | `make windows-installer-signed` (or `make sign-windows-bin BIN=…`) |

Everything is signed on Linux with **jsign** over PKCS#11 (see "Three facts"
above); `osslsigncode` only verifies. The Inno `SignTool` directive is not
used — post-build signing is simpler and works from CI.

> **Sign the payload BEFORE building the installer.** ISCC packs whatever is
> sitting in `windows-installer/`, so signing only the finished `Setup.exe`
> ships a signed installer that installs an **unsigned** program: the publisher
> shows on the download and then disappears the moment the user runs it.
> `make windows-installer-signed` enforces the order — payload, then ISCC, then
> installer — in a single SimplySign session.

```bash
# One command, one cloud login: signs mercury-ui.exe, builds the installer,
# signs the installer.  ISCC is a Windows tool; point at it via Wine, or leave
# ISCC unset to stop after the payload and get the two remaining commands.
make windows-installer-signed \
     ISCC='wine ~/.wine/drive_c/Program Files (x86)/Inno Setup 6/ISCC.exe'
```

Note the installer packs **only `mercury-ui.exe`** — it is the single-binary
build with the modem core linked in (`-tags mercury_embedded`), so there is no
separate console `mercury.exe` inside the installer. The console binary ships
in the ZIP.

`installer.iss` carries its own `MyAppVersion`; keep it in step with
`MERCURY_VERSION` in `common/mercury_version.h` or the artifact name and the
build will disagree.>

## Setup: SimplySign Desktop + signing scripts

The SimplySign Desktop binary is installed at `/opt/SimplySignDesktop`. The
signing scripts are **in the repo** at `code-signing/` (see its `README.md`);
the **secrets stay outside** the repo (see "Secrets" above):

| File | Where | Purpose |
|---|---|---|
| `sign-lib.sh` / `sign.sh` / `sign-logout.sh` | `code-signing/` (repo) | login-once + jsign sign + teardown |
| otpauth URI (TOTP seed) | `~/.config/mercury-signing/otpauth.txt` (**secret**) | `CERTUM_OTP_URI_FILE` |
| account e-mail | shell env (**secret**) | `CERTUM_EMAIL` |
| jsign jar | `~/.local/share/jsign.jar` | `JSIGN_JAR` |
| public certificate | `~/files/documents/windows_certum/*.pem` | verification only (not secret) |

## Parameters reference (Makefile)

| Variable | Default | Meaning |
|---|---|---|
| `WIN_SIGN_PFX` | *(empty)* | Path to a local PKCS#12 (`.pfx`) cert+key. **When set**: uses `osslsigncode -pkcs12` (Mode B, test only). **When empty**: uses SimplySign cloud via `SIGN_SCRIPT` (Mode A). |
| `WIN_SIGN_PASS` | *(empty)* | Password for the `.pfx` (only used when `WIN_SIGN_PFX` is set). |
| `WIN_SIGN_TS` | `http://timestamp.digicert.com` | RFC-3161 TSA (Mode B). Certum's TSA is `http://time.certum.pl` (Mode A uses `CERTUM_TSA`, same default). |
| `WIN_SIGN_NAME` | `Mercury HF Modem` | Signature description (`-n`, Mode B). |
| `WIN_SIGN_URL` | `https://github.com/Rhizomatica/mercury` | Signature info URL (`-i`, Mode B). |
| `SIGN_SCRIPT` | `$(CURDIR)/code-signing/sign.sh` | In-repo SimplySign automation. Mode A runs it **only when `CERTUM_EMAIL` is set** (opt-in), so plain builds stay unsigned. |
| `SIGN_LOGOUT` | `$(CURDIR)/code-signing/sign-logout.sh` | Session teardown, run after `windows-zip-signed`. |

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
