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
wants. **Choose your own password** and keep it with the file:

```sh
cd ~/.config/mercury-signing
openssl x509 -inform DER -in developerID_application.cer -out apple_developer_id.pem
openssl pkcs12 -export \
  -inkey apple_developer_id.key \
  -in apple_developer_id.pem \
  -out apple_developer_id.p12
chmod 600 apple_developer_id.p12
```

Check it is the certificate you think it is — the common name must start with
`Developer ID Application:`:

```sh
openssl pkcs12 -in apple_developer_id.p12 -nodes -passin pass:YOURPASS \
  | openssl x509 -noout -subject -dates
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

Notarization takes a few minutes; `--staple` writes the ticket into the `.dmg`
so it validates even when the user is offline. Verify on a Mac:

```sh
codesign --verify --deep --strict --verbose=2 /Applications/Mercury.app
spctl -a -t open --context context:primary-signature -v Mercury-*.dmg
```

## 5. Wire it into CI

The release workflow signs and notarizes the macOS artifact when three
repository secrets are present. Set them from the files above — the `.p12` and
the key JSON are binary/multiline, so base64 them:

```sh
cd ~/.config/mercury-signing
gh secret set MACOS_SIGN_P12_BASE64   --repo Rhizomatica/mercury < <(base64 -w0 apple_developer_id.p12)
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
| `rcodesign` rejects the `.p12` | wrong certificate type — must be *Developer ID Application*, check with the `openssl x509 -subject` command in §2 |
| Notarization rejected: "The signature does not include a secure timestamp" | signing ran without network; rcodesign timestamps by default, so this means the timestamp server was unreachable |
| Notarization rejected: "The executable does not have the hardened runtime enabled" | `--code-signature-flags runtime` missing — the Makefile always passes it, so this means something was signed outside `macos_sign` |
| Gatekeeper still warns after notarizing | the ticket was not stapled, or the `.dmg` was rebuilt after stapling — staple last |
| `Team Keys` tab absent in App Store Connect | your Apple ID is not Account Holder/Admin on the team |
