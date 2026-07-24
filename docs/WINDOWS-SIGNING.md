# Windows code signing (Authenticode)

Mercury's Windows binaries and installer can be Authenticode-signed. Two
approaches:

| Mode | Tool | When |
|---|---|---|
| **A) SimplySign cloud** | `sign.sh` (Xvfb + xdotool + PKCS#11) | Default for CI / developer workstations — no `.pfx` file needed |
| **B) Local .pfx** | `osslsigncode -pkcs12` | Self-signed test cert or OV/EV hardware token |
| **C) Inno Setup** | `signtool.exe` on Windows | Signing the installer (requires Windows VM or physical machine) |

**Mode A (SimplySign cloud) is the production path.** The certificate lives in
Certum's cloud HSM; there is no `.pfx` file. The `sign.sh` script automates the
SimplySign Desktop GUI (Xvfb + xdotool), types the TOTP code into the login form,
waits for the PKCS#11 token to activate, then signs with `osslsigncode -pkcs11`.

## Prerequisites

```bash
# 6 packages (Debian 13):
sudo apt-get install -y xvfb fluxbox xdotool opensc osslsigncode stalonetray
```

SimplySign Desktop must be installed at `/opt/SimplySignDesktop` (pre-built
binary from Certum; bundles its own Qt 5.9.2 + OpenSSL 1.0.0 — no system Qt
or OpenSSL packages needed).

## Mode A: SimplySign cloud signing (default)

The Makefile automatically uses this when `WIN_SIGN_PFX` is unset and the
`sign.sh` script is available:

```bash
# Sign mercury.exe (already built)
make sign

# Sign an arbitrary binary
make sign-bin BIN=mercury-ui.exe

# Build + sign + zip in one step
make windows-zip-signed
```

Under the hood this runs `~/files/MYSELF/code-signing/sign.sh`, which:
1. Starts Xvfb :99 + fluxbox (virtual display for the GUI)
2. Launches SimplySign Desktop with `USER=rafael2k` (prevents OpenSSL crash)
3. Waits for the "SimplySign Desktop" login window to appear
4. Generates a fresh TOTP code from `otpauthuri.txt`
5. Uses xdotool to type email + TOTP + click Login
6. Handles the "Logon successful" dialog
7. Signs with `osslsigncode` via PKCS#11
8. Verifies the signature

## What can be signed, and where

| Artifact | Built on | Signed on | Tool |
|---|---|---|---|
| `mercury.exe`, `mercury-ui.exe` | Linux (mingw) | Linux | `osslsigncode` via PKCS#11 (`make sign`) |
| `Mercury_*_Setup.exe` (installer) | Windows or Wine (Inno Setup ISCC) | Linux | `osslsigncode` via PKCS#11 (`make sign-bin BIN=Setup.exe`) |

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
make sign-bin BIN=Mercury_1.9.10_Setup.exe
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
