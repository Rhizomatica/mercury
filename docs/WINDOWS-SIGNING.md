# Windows code signing (Authenticode)

Mercury's Windows binaries and installer can be Authenticode-signed. This guide
covers **self-signed signing for testing the pipeline**; the same steps apply to
a real certificate — only the cert source changes.

> ⚠️ **A self-signed certificate proves the mechanics only.** It does *not*
> clear the SmartScreen / "unknown publisher" warning on machines that don't
> trust it. For public releases use an OV/EV code-signing certificate or a cloud
> signing service (e.g. **Azure Trusted Signing**, ~$10/mo, CI-friendly).

## What can be signed, and where

| Artifact | Built on | Sign with |
|---|---|---|
| `mercury.exe`, `mercury-ui.exe` | Linux (mingw cross-build) | `osslsigncode` on Linux, **or** Inno (`signonce`) on Windows |
| `Mercury_HF_Modem_Setup.exe` (installer) | Windows (Inno Setup) | Inno `SignTool` directive |

Because the payload `.exe` files are cross-built on Linux, you can sign them
there and let Inno sign only the installer — or let Inno sign everything on the
Windows side. Both are wired up.

## 1. Make a self-signed test certificate

**On Linux** (openssl):
```bash
cd windows-installer
./gen-selfsigned-cert.sh                 # -> mercury-selfsign.pfx (password: mercury)
```

**On Windows** (PowerShell) — equivalent:
```powershell
$c = New-SelfSignedCertificate -Type CodeSigningCert `
       -Subject "CN=Rhizomatica Mercury (TEST)" `
       -CertStoreLocation Cert:\CurrentUser\My
Export-PfxCertificate -Cert $c -FilePath mercury-selfsign.pfx `
       -Password (ConvertTo-SecureString -String "mercury" -Force -AsPlainText)
```

`.pfx` / `.p12` files are git-ignored — never commit a signing key.

## 2a. Sign the binaries on Linux (osslsigncode)

The Makefile signs the cross-built `.exe` when `WIN_SIGN_PFX` is set (unset =
unsigned build, the default — no behaviour change):

```bash
make windows-zip \
     WIN_SIGN_PFX=$PWD/windows-installer/mercury-selfsign.pfx \
     WIN_SIGN_PASS=mercury
```

Manual single-file equivalent:
```bash
osslsigncode sign -pkcs12 mercury-selfsign.pfx -pass mercury \
    -n "Mercury HF Modem" -i https://github.com/Rhizomatica/mercury \
    -h sha256 -ts http://timestamp.digicert.com \
    -in mercury.exe -out mercury-signed.exe
osslsigncode verify mercury-signed.exe
```
`-ts` adds an RFC-3161 trusted timestamp so signatures stay valid after the cert
expires. On a self-signed cert `verify` reports the chain as untrusted (expected)
but confirms the embedded digest matches the file.

## 2b. Sign via Inno Setup on Windows

`installer.iss` gates signing behind the `SIGN` preprocessor define, so the
default build is unchanged. Compile signed by registering a sign tool named
`mercury` and passing `/DSIGN`:

```bat
ISCC.exe /DSIGN ^
  /Smercury="signtool sign /f mercury-selfsign.pfx /p mercury /fd sha256 /tr http://timestamp.digicert.com /td sha256 $f" ^
  windows-installer\installer.iss
```

With `/DSIGN` Inno:
- signs `mercury-ui.exe` inside the installer (`signonce` flag), and
- signs the generated `Mercury_HF_Modem_Setup.exe` and its uninstaller
  (`SignTool` / `SignedUninstaller`).

`osslsigncode` can substitute for `signtool` in the same command if you build the
installer under Wine.

## Parameters reference

**Makefile (`osslsigncode` path)** — set on the `make windows-zip` command line
or the environment. Signing is skipped entirely unless `WIN_SIGN_PFX` is set.

| Variable | Default | Meaning |
|---|---|---|
| `WIN_SIGN_PFX` | *(empty)* | Path to the PKCS#12 (`.pfx`) cert+key. **Empty ⇒ no signing** (default). |
| `WIN_SIGN_PASS` | *(empty)* | Password for the `.pfx`. |
| `WIN_SIGN_TS` | `http://timestamp.digicert.com` | RFC-3161 timestamp authority URL. |
| `WIN_SIGN_NAME` | `Mercury HF Modem` | Signature description (`-n`). |
| `WIN_SIGN_URL` | `https://github.com/Rhizomatica/mercury` | Signature info URL (`-i`). |

**Inno Setup (`installer.iss`)** — controlled by the ISPP preprocessor:

| Define / directive | Effect |
|---|---|
| `/DSIGN` | Enables signing: adds `SignTool=mercury`, `SignedUninstaller=yes`, and the `signonce` flag on `mercury-ui.exe`. Omit it ⇒ unsigned build (default). |
| `/Smercury="<cmd> $f"` | Registers the sign tool named `mercury`; `$f` expands to each file to sign. |

## 3. Trust the test cert locally (optional)

To silence the warning *on your own test machine* only, import the cert into
**Trusted Root Certification Authorities** and **Trusted Publishers**
(`certmgr.msc`, LocalMachine). Do this only on throwaway/test systems.

## Moving to a real certificate

Swap the self-signed `.pfx` for the real credential — everything else (Makefile
`WIN_SIGN_*`, Inno `/DSIGN`) is identical:
- **Azure Trusted Signing** — cloud HSM, CI-native, immediate SmartScreen trust;
  requires organization identity validation. Recommended.
- **OV cert** on a hardware token — reputation builds over time.
- **EV cert** — instant SmartScreen reputation; hardware token.

For CI, store the cert (base64 `.pfx` + password) or the cloud-signing service
credentials as repository secrets and set `WIN_SIGN_PFX`/`WIN_SIGN_PASS` (or the
Inno `/DSIGN` sign-tool command) from them in the release workflow.
