# macOS code signing and notarization

Gatekeeper on a current macOS refuses to open a downloaded `.app` unless it is
**signed with a Developer ID Application certificate** *and* **notarized by
Apple**. Signing alone is not enough — an unnotarized build still gets
"Apple could not verify ... free of malware", and the only way past it is a
right-click → Open, which most people will not do and should not be asked to.

Both steps are done with [`rcodesign`](https://github.com/indygreg/apple-platform-rs)
(the `apple-codesign` crate), not Apple's `codesign`/`notarytool`. It signs
Mach-O binaries, bundles, `.dmg` images and `.pkg` archives, and notarizes and
staples, **without a Mac, Xcode or a keychain**. See the rationale in the
Makefile's *macOS code signing* section.

`hdiutil` still needs macOS to build the `.dmg` itself; what moves off the Mac
is signing and notarization.

---

## What you need

| thing | what it is | where it lives |
|---|---|---|
| Developer ID Application certificate | signs the CLI, the `.app` and the `.dmg` | `~/.config/mercury-signing/apple_developer_id.p12` |
| its private key | generated here, never leaves the machine | `~/.config/mercury-signing/apple_developer_id.key` |
| App Store Connect API key | authenticates the notarization upload | `~/.config/mercury-signing/apple_notary_key.json` |

**Keep all of it OUTSIDE the git tree**, same rule as
[WINDOWS-SIGNING.md](WINDOWS-SIGNING.md). `.gitignore` blocks `*.p12` as a
backstop, but the rule is that the material never enters the repository at all.
The paths below are a suggestion; anywhere outside the tree works, and the
maintainer's copies live in `hermes/apple/` alongside Apple's public
intermediate certificates (`certs/DeveloperIDG2CA.cer` and the WWDR CAs), which
are **not** secret and are what lets a signing tool build the full chain.

A Developer ID certificate can only be created by the team's **Account Holder**
(Admins cannot), and Apple caps them at **5 per team** — they are not
disposable, so keep the `.p12` and its password somewhere you will still have
them in three years. Losing the private key means revoking and reissuing.

---

## 1. Generate a key and CSR (on Linux — no Mac needed)

Apple's own instructions say to use Keychain Access on a Mac. You do not have
to; a CSR is a CSR. Apple requires RSA 2048.

```sh
mkdir -p ~/.config/mercury-signing && chmod 700 ~/.config/mercury-signing
cd ~/.config/mercury-signing
openssl req -new -newkey rsa:2048 -nodes \
  -keyout apple_developer_id.key \
  -out apple_developer_id.csr \
  -subj "/CN=Rhizomatica/emailAddress=you@example.org/C=BR"
chmod 600 apple_developer_id.key
```

The subject barely matters — Apple overwrites it, and the issued certificate
comes back as `Developer ID Application: <Team Name> (<TEAMID>)`. The `.csr` is
**not secret**; the `.key` is the whole certificate's security.

## 2. Get the certificate from Apple

At <https://developer.apple.com/account/resources/certificates>:

1. **Certificates** → **+**
2. Under **Software**, choose **Developer ID Application**
   — *not* "Developer ID Installer" (that signs `.pkg`, which we do not ship),
   and *not* "Apple Development"/"Apple Distribution" (those are for the App
   Store and Gatekeeper will not accept them for direct download).
3. If it asks for a **Profile Type**, choose **G2 Sub-CA**.
4. Upload `apple_developer_id.csr`.
5. Download the issued `developerID_application.cer`.

Then combine the certificate with the private key into the `.p12` rcodesign
wants. **Two details here are not optional, and both were found the hard way by
signing a real binary and asking Apple's own `codesign` what it thought.**

**1. The `.p12` must hold the leaf ONLY — do not add the intermediate.**
rcodesign takes the *first* certificate in the bundle as the signing
certificate, and it reads an OpenSSL-written bundle CA-first. Adding
`-certfile certs/DeveloperIDG2CA.pem` therefore makes it sign with the
*Developer ID Certification Authority*, producing `TeamIdentifier=G2`,
`Authority=(unavailable)` and a signature Apple rejects as
`invalid signature (code or signature have been modified)`. rcodesign registers
Apple's CA chain by itself:

```
automatically registered Apple CA certificate: Developer ID Certification Authority
automatically registered Apple CA certificate: Apple Root CA
```

so a leaf-only bundle yields the full `Authority` chain anyway.

**2. It must use the legacy PKCS#12 algorithms.** OpenSSL 3 defaults to
AES-256-CBC + PBKDF2, which rcodesign's PFX parser cannot read — and the error
it gives points at entirely the wrong thing:

```
Error: incorrect password given when decrypting PFX data
```

That message means *unsupported encryption*, not a wrong password. Measured:

| bundle | password | rcodesign |
|---|---|---|
| legacy (SHA1/3DES) | empty | accepted |
| legacy (SHA1/3DES) | set | accepted |
| OpenSSL 3 default | set | rejected |
| OpenSSL 3 default | empty | rejected |

**An empty password is fine** — the table shows it is the algorithm that
matters, not the password. It is what this project uses: the key generated in
§1 is already unencrypted on disk beside the bundle, and in CI the `.p12` is a
bearer credential inside a secret store either way, so a password protects
nothing unless that private key is also encrypted or deleted.

```sh
cd ~/.config/mercury-signing
openssl x509 -inform DER -in developerID_application.cer -out apple_developer_id.pem
openssl pkcs12 -export \
  -inkey apple_developer_id.key \
  -in apple_developer_id.pem \
  -name "Developer ID Application: <Team Name>" \
  -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 \
  -out apple_developer_id.p12 \
  -passout pass:
chmod 600 apple_developer_id.p12
```

Verify it end to end rather than by inspection — sign a throwaway Mach-O and
let Apple judge it. This is the only check that catches both traps above:

```sh
cp /bin/echo /tmp/sigtest && chmod +w /tmp/sigtest
rcodesign sign --p12-file apple_developer_id.p12 --p12-password "" \
  --code-signature-flags runtime /tmp/sigtest
codesign -dv --verbose=4 /tmp/sigtest 2>&1 | grep -E "^Authority|TeamIdentifier"
codesign --verify --strict --verbose=2 /tmp/sigtest
```

Correct output names your organisation, not the CA, and ends with
`valid on disk` / `satisfies its Designated Requirement`:

```
Authority=Developer ID Application: <Your Org> (<TEAMID>)
Authority=Developer ID Certification Authority
Authority=Apple Root CA
TeamIdentifier=<TEAMID>
```

Apple's Developer ID certificates are valid for 5 years. Note the expiry: a
lapsed certificate does not break already-notarized downloads (the notarization
ticket is what Gatekeeper checks), but it stops you signing new ones.

## 3. Get an App Store Connect API key (for notarization)

This is **not** in the developer portal's *Keys* tab — those are service keys
(APNs, DeviceCheck). Notarization uses an App Store Connect team key:

1. <https://appstoreconnect.apple.com> → **Users and Access** → **Integrations**
   → **App Store Connect API** → **Team Keys**
2. **+**, give it a name, role **Developer** (enough to notarize)
3. Download `AuthKey_<KEYID>.p8` — **Apple lets you download it once.**
4. Note the **Issuer ID** (a UUID at the top of the page) and the **Key ID**.

Encode all three into the single JSON file rcodesign uses. rcodesign has **no
Homebrew formula**; install the pinned pre-built binary (the version the
Makefile documents as verified) or build it with `cargo install apple-codesign`:

```sh
V=0.28.0
curl -fsSLO "https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign%2F$V/apple-codesign-$V-x86_64-unknown-linux-musl.tar.gz"
tar xzf apple-codesign-$V-x86_64-unknown-linux-musl.tar.gz
install -m755 apple-codesign-*/rcodesign ~/.local/bin/
```

Then:

```sh
rcodesign encode-app-store-connect-api-key \
  -o ~/.config/mercury-signing/apple_notary_key.json \
  <ISSUER_ID> <KEY_ID> AuthKey_<KEYID>.p8
chmod 600 ~/.config/mercury-signing/apple_notary_key.json
```

## 4. Sign and notarize locally

`hdiutil` is macOS-only, so the `.dmg` itself is built on a Mac; the signing
happens through the Makefile either way:

```sh
make fyne-ui-macos-universal-dmg \
  MACOS_SIGN_P12=~/.config/mercury-signing/apple_developer_id.p12 \
  MACOS_SIGN_P12_PASSWORD='...'

make macos-notarize-dmg \
  MACOS_NOTARY_KEY=~/.config/mercury-signing/apple_notary_key.json
```

Signing is **opt-in**: with `MACOS_SIGN_P12` unset the build still completes and
prints `WARNING: ... is unsigned`, so ordinary developer builds are unchanged.

Notarization is **slow and variable** — a 52 MB `.dmg` was still `InProgress`
past 600 s, which is rcodesign's default wait, so the Makefile passes
`--max-wait-seconds $(MACOS_NOTARY_WAIT)` (default 3600). Hitting the limit is
not a rejection: the submission carries on server-side, and
`rcodesign notary-wait <submission-id>` resumes waiting on it. `--staple` writes
the ticket into the `.dmg` so it validates even when the user is offline. Verify on a Mac:

```sh
codesign --verify --deep --strict --verbose=2 /Applications/Mercury.app
spctl -a -t open --context context:primary-signature -v Mercury-*.dmg
```

## 4b. Entitlements and what actually gets stapled

Two things are easy to get wrong here, and **notarization passes either way** —
they fail on the user's machine instead.

**The hardened runtime needs entitlements.** Notarization requires the hardened
runtime, and the runtime denies protected resources by default. An
`Info.plist` usage string only supplies the prompt text; it grants nothing. So
`macos/entitlements.plist` declares what Mercury genuinely needs:

| entitlement | why |
|---|---|
| `com.apple.security.device.audio-input` | capture audio from the radio — without it the modem cannot hear |
| `com.apple.security.device.usb` | CM108 USB HID PTT (`radio_io/cm108_ptt.c`) |

Nothing else: every entry widens what the signed binary may do. Check the
result rather than trusting the flag —

```sh
codesign -d --entitlements - /Volumes/Mercury*/Mercury.app
```

**Staple the `.app`, not just the `.dmg`.** A ticket stapled to the `.dmg`
covers the `.dmg`. The app the user drags to /Applications carries no ticket,
so Gatekeeper has to ask Apple online at first launch — the wrong failure mode
for a station on a poor or absent link. The recipe therefore notarizes and
staples the `.app` **before** `hdiutil` seals it (a `.dmg` is read-only
afterwards, so it is the last chance), then notarizes and staples the `.dmg`.
That is two Apple round trips; `MACOS_STAPLE_APP=0` skips the first.

```sh
xcrun stapler validate /Volumes/Mercury*/Mercury.app   # must NOT say "does not have a ticket"
```

## 5. Wire it into CI

The release workflow signs and notarizes the macOS artifact when three
repository secrets are present. Set them from the files above — the `.p12` and
the key JSON are binary/multiline, so base64 them:

```sh
cd ~/.config/mercury-signing
gh secret set MACOS_SIGN_P12_BASE64   --repo Rhizomatica/mercury < <(base64 -w0 apple_developer_id.p12)
# Only if the .p12 has a password.  The job treats it as optional, so an
# unset secret means "empty password" and is a valid configuration.
gh secret set MACOS_SIGN_P12_PASSWORD --repo Rhizomatica/mercury   # prompts, does not echo
gh secret set MACOS_NOTARY_KEY_BASE64 --repo Rhizomatica/mercury < <(base64 -w0 apple_notary_key.json)
```

With the secrets absent the macOS job **fails loudly** rather than publishing an
unsigned `.dmg`: an unsigned build on a release page is worse than no build,
because it teaches people to click through Gatekeeper warnings.

---

## Troubleshooting

| symptom | cause |
|---|---|
| `Error: incorrect password given when decrypting PFX data` | **Not a password problem.** The `.p12` uses OpenSSL 3's default AES-256/PBKDF2, which rcodesign cannot read. Rebuild it with `-keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1` (§2). |
| Signature has `TeamIdentifier=G2` and `Authority=(unavailable)`; `codesign --verify` says `invalid signature (code or signature have been modified)` | The `.p12` contains the intermediate as well as the leaf, so rcodesign signed with the CA. Rebuild it leaf-only, without `-certfile` (§2). |
| `rcodesign` rejects the certificate type | must be *Developer ID Application*, check with the `openssl x509 -subject` command in §2 |
| Notarization rejected: "The signature does not include a secure timestamp" | signing ran without network; rcodesign timestamps by default, so this means the timestamp server was unreachable |
| Notarization rejected: "The executable does not have the hardened runtime enabled" | `--code-signature-flags runtime` missing — the Makefile always passes it, so this means something was signed outside `macos_sign` |
| Gatekeeper still warns after notarizing | the ticket was not stapled, or the `.dmg` was rebuilt after stapling — staple last |
| `Team Keys` tab absent in App Store Connect | your Apple ID is not Account Holder/Admin on the team |
