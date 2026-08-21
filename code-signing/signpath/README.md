# SignPath code signing (OSS program)

Signing the Windows binaries through the [SignPath](https://signpath.io) open
source program: no cloud-cert login gymnastics, no secrets beyond one API
token, and the certificate itself never leaves SignPath's HSM.

Status: **trial** — the workflow signs with the self-signed *test* certificate
the OSS organization provides. The Certum SimplySign release flow
(`release.yml`, `code-signing/sign.sh`) is untouched while this is validated.

```
artifact-configs/mercury-windows.xml   artifact configuration: ZIP with mercury.exe + mercury-ui.exe
artifact-configs/mercury-setup.xml     artifact configuration: ZIP with mercury-setup.exe (installer)
../../.github/workflows/signpath-test.yml   the trial workflow
```

## One-time setup (portal, done by a maintainer with the OSS org invite)

1. **Accept the SignPath organization invitation** (from the approval email).

2. **Trusted Build System**: Organization → add *Trusted Build System* →
   **GitHub.com**. Link it to the project in the next step.

3. **Project + signing policy**: create project `mercury` and a signing
   policy (e.g. `test-signing`) that uses the test certificate.

4. **Artifact configurations**: in the project, add two artifact
   configurations and paste the XML from `artifact-configs/`:
   - `mercury-windows` ← `mercury-windows.xml`
   - `mercury-setup`  ← `mercury-setup.xml`
   (Alternative: upload a sample ZIP of the two exes and let SignPath
   generate the config, then review it.)

5. **API token**: in the SignPath portal create an API token for a user with
   *submitter* permission on the project/policy.

6. **GitHub repo secret**: add the token as `SIGNPATH_API_TOKEN`
   (Settings → Secrets and variables → Actions).

7. **Workflow placeholders**: replace `<ORGANIZATION_ID>`, `<PROJECT_SLUG>`
   and `<POLICY_SLUG>` in `.github/workflows/signpath-test.yml` with the real
   portal values (visible in the portal; project slug is what you named it).

8. **SignPath GitHub App** (recommended): install
   [SignPath](https://github.com/apps/signpath) on the Rhizomatica org with
   access to `mercury`. Required if we later enable source/build policies;
   it also improves the origin-verification signal for the OSS review.

## Run the trial

- Actions → *SignPath test* → **Run workflow** on the `signpath-test` branch.
  - Sign only the two exes: leave *build_installer* off.
  - Also sign the installer: tick *build_installer* (needs Wine + Inno; the
    signed payloads are staged into the installer before the Setup.exe is
    submitted for signing).
- Expected: two (or three) `signpath-signed` / `signpath-setup-signed`
  artifacts whose signatures verify with `osslsigncode`. Chain validation
  will fail for the self-signed test cert — that is expected.

## Production certificate

After SignPath reviews the setup (origin verification), they import the real
release certificate into the organization.

**Production signing requests require manual approval in the SignPath UI.**
This is expected for SignPath Foundation OSS certificates — they verify what is
being signed and from which build — and it is the one behavioural difference
from the test certificate, which signs unattended. Two consequences:

* The action waits for a *person*. Its default
  `wait-for-completion-timeout-in-seconds` is **600** (10 minutes), which will
  expire on most real releases. `.github/workflows/signpath-release.yml` raises
  it via the `approval_timeout_s` input (default 7200) and leaves the job cap at
  the 6 h GitHub maximum, so the action's own timeout is what fires — with a
  usable error — rather than the runner being killed mid-wait.

* A release needs **two approvals**: the binaries, then the installer, because
  the installer has to contain already-signed payloads. SignPath emails the
  approvers for each, and the request URLs are written to the job summary.
  Worth asking SignPath whether one artifact configuration can cover the
  installer *and* its embedded exes, which would halve that.

`signpath-release.yml` is the production variant of `signpath-test.yml`. Same
proven shape — build unsigned, sign, build the installer from signed payloads,
sign the installer — with the approval-related timeouts, the policy slug as an
input (default `release-signing`), and one extra guard: the verify step **fails**
if the resulting signature is self-signed, since that means the policy is still
pointed at the trial certificate and the release would ship untrusted.

Remaining steps once the certificate lands:

1. Create a signing policy bound to the production certificate (slug
   `release-signing`, or pass `signing_policy`).
2. Run *SignPath release* manually and approve both requests; confirm the
   artifacts verify.
3. Then wire it into `.github/workflows/release.yml`, replacing the Certum jobs.
4. Keep `code-signing/sign.sh` + the Certum secrets as a manual fallback.

See [SignPath docs](https://about.signpath.io/documentation/) — the GitHub
integration page and the artifact-configuration reference.
