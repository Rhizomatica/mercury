# Mercury documentation

Start with the [project README](../README.md) if you are new. This page is the
map of everything else.

Documentation here falls into three kinds, and it helps to know which you are
reading: **reference** describes how Mercury behaves today and is kept current;
**guides** are task-oriented; **findings** are dated records of an
investigation, kept because the reasoning and the negative results are worth
having — they are not updated as the code moves on.

## Reference — how Mercury works

| Document | What it covers |
|---|---|
| [ARQ.md](ARQ.md) | The ARQ data link: state machine, frame formats, timers, mode ladder, and the OTA tuning guide. The main architecture document. |
| [TNC.md](TNC.md) | Every TNC command and async status on the control socket. What you need to drive Mercury from your own software. |
| [MODES.md](MODES.md) | All modulation modes with measured bandwidth, payload, FEC and delivery rates — including the Mercury-specific DATAC15/16/17 and QAM16C2. |
| [watterson_model.md](watterson_model.md) | The HF channel simulator used for every measurement in these docs, and how its noise axis is calibrated. |

## Guides — doing a particular thing

| Document | What it covers |
|---|---|
| [RETICULUM.md](RETICULUM.md) | Carrying a Reticulum mesh over HF, via the broadcast port or an ARQ backbone. |
| [MACOS-UNIVERSAL.md](MACOS-UNIVERSAL.md) | Building the self-contained universal macOS app with vendored static Hamlib. |
| [WINDOWS-SIGNING.md](WINDOWS-SIGNING.md) | Authenticode signing for Windows releases. |
| [MACOS-VM-GLFW-SOFTWARE-OPENGL.md](MACOS-VM-GLFW-SOFTWARE-OPENGL.md) | Running the Fyne UI on a macOS VM with no GPU. |
| [SANITIZERS.md](SANITIZERS.md) | ASan/UBSan and TSan builds. |
| [FUZZING.md](FUZZING.md) | Fuzzing the frame parsers. |

## Findings — why things are the way they are

Point-in-time investigation records. Each answers a question that was open at
the time; the conclusions shaped the code, and the methods are reusable. Read
them for reasoning, not for current behaviour.

| Document | The question it answered |
|---|---|
| [HARQ-FINDINGS.md](HARQ-FINDINGS.md) | Does soft-combining repeated frames actually rescue a link at the fading cliff? (Yes — roughly half the frames, where single-shot delivers none.) |
| [FADE-CLIFF-DECISION.md](FADE-CLIFF-DECISION.md) | Should the S1 fade-cliff fix be merged? Simulation evidence behind the decision. |
| [SPEED-REGRESSION-FINDINGS.md](SPEED-REGRESSION-FINDINGS.md) | Why a pre-2.0 build was slower than v1.9.9 on the bench — FEC under fading and gear-shift oscillation, not the guard intervals everyone suspected. |
| [OTA-PHASE-A-SNR-CALIB.md](OTA-PHASE-A-SNR-CALIB.md) | Why the mode ladder stuck at the bottom: per-mode SNR estimation bias. |
| [broadcast-length-prefix-root-cause.md](broadcast-length-prefix-root-cause.md) | Why VarAC broadcasts would not decode. |
| [ARDOP-IDEAS.md](ARDOP-IDEAS.md) | What is worth borrowing from ARDOP. |
| [PLAN-arq-robustness-tx-gain.md](PLAN-arq-robustness-tx-gain.md) | Improvements proposed from Gary K7EK's review. |

## Elsewhere

- **Rendered HTML**: https://rhizomatica.github.io/mercury/
- **Web interface**: https://rhizomatica.github.io/mercury/app/ (source in [app/](app/))
- **Configuration**: every setting with its default is in
  [mercury.ini.example](../mercury.ini.example)
- **Mailing list**: https://lists.riseup.net/www/info/hermes-general

## A note on the measurements

Numbers in these documents come from a calibrated bench, not from estimates.
The channel simulator is checked against published upstream figures before its
results are trusted for our own modes — `docs/MODES.md` records that
cross-check explicitly. Where a result was negative, it is written down as a
negative result rather than removed; several of the most useful pages here are
records of something that did **not** work.
