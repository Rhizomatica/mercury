# Mercury documentation

Start with the [project README](../README.md) if you are new. This page is the
map of everything else.

Documentation here falls into three kinds, and it helps to know which you are
reading: **reference** describes how Mercury behaves today and is kept current;
**guides** are task-oriented; **findings** are dated records of an
investigation, kept because the reasoning and the negative results are worth
having — they are not updated as the code moves on.

## Releases

| Document | What it covers |
|---|---|
| [RELEASE-1.9.19.md](RELEASE-1.9.19.md) | **1.9.19 does not talk to 1.9.18 on the air.** MFSK moved to the centre of the passband (1500 Hz), what an operator sees with mixed versions, and what was tested on air. |
| [RELEASE-1.9.18.md](RELEASE-1.9.18.md) | What changed since 1.9.17 and what to check before upgrading, with 1.9.17 and 1.9.18 measured side by side on real modems and on air. |
| [UPGRADING-TO-1.9.18.md](UPGRADING-TO-1.9.18.md) | **Read before upgrading:** 1.9.18 does not talk to 1.9.x on the air. What an operator sees with mixed versions, how to upgrade a network, and what was tested before the release. |

## Reference — how Mercury works

| Document | What it covers |
|---|---|
| [CAROUSEL-ARQ.md](CAROUSEL-ARQ.md) | The ARQ data link: the connect (protocol version 1), the erasure-coded carousel that every session runs, the MFSK floor, the bandwidth caps, and the wire formats. The main architecture document. |
| [CAROUSEL-SPEC.md](CAROUSEL-SPEC.md) | The carousel specified rule by rule, from the code: state, messages, timers, transitions and invariants. |
| [CAROUSEL-TURNS.md](CAROUSEL-TURNS.md) | Who may key when: the caller's timers and the callee's slots, and the timed TLA+ model behind them. |
| [ARQ.md](ARQ.md) | The connection FSM and the 1.9.x stop-and-wait data plane -- mostly history now; read CAROUSEL-ARQ.md for the current data link. |
| [TNC.md](TNC.md) | Every TNC command and async status on the control socket. What you need to drive Mercury from your own software. |
| [WEBSOCKET_API.md](WEBSOCKET_API.md) | The WebSocket UI API end to end: transport, every command, every event, reply and error, and the binary waterfall frame. |
| [MODES.md](MODES.md) | All modulation modes by index, where each is used, and measured bandwidth, payload, FEC and delivery rates — including the Mercury-specific DATAC15/16/17, MFSK and QAM16C2. |
| [BROADCAST-WIRE-FORMAT.md](BROADCAST-WIRE-FORMAT.md) | The broadcast plane on the wire: frame layout, RaptorQ symbol sizing, and mode agreement. |
| [watterson_model.md](watterson_model.md) | The HF channel simulator used for every measurement in these docs, and how its noise axis is calibrated. |

## Guides — doing a particular thing

| Document | What it covers |
|---|---|
| [RETICULUM.md](RETICULUM.md) | Carrying a Reticulum mesh over HF, via the broadcast port or an ARQ backbone. |
| [JNOS.md](JNOS.md) | JNOS 2.0 over Mercury's VARA-compatible TNC: BBS to BBS, and telnet to the other station's Linux with no IP on the air. Bench setup, configs, test transcripts and the problems found. |
| [BROADCAST-FILE.md](BROADCAST-FILE.md) | Broadcasting a file to whoever is listening, with no return path: the RaptorQ carousel, `bcast_file_tool`, and which modes can carry it. |
| [MACOS-UNIVERSAL.md](MACOS-UNIVERSAL.md) | Building the self-contained universal macOS app with vendored static Hamlib. |
| [WINDOWS-SIGNING.md](WINDOWS-SIGNING.md) | Authenticode signing for Windows releases. |
| [MACOS-VM-GLFW-SOFTWARE-OPENGL.md](MACOS-VM-GLFW-SOFTWARE-OPENGL.md) | Running the Fyne UI on a macOS VM with no GPU. |
| [SANITIZERS.md](SANITIZERS.md) | ASan/UBSan and TSan builds. |
| [MACOS-SIGNING.md](MACOS-SIGNING.md) | Apple notarization and signing for macOS releases. |
| [WEBSOCKET_STATUS.md](WEBSOCKET_STATUS.md) | The WebSocket status feed (`-G`) that the Qt/web UI consumes. |
| [FUZZING.md](FUZZING.md) | Fuzzing the frame parsers. |

## Findings — why things are the way they are

Point-in-time investigation records. Each answers a question that was open at
the time; the conclusions shaped the code, and the methods are reusable. Read
them for reasoning, not for current behaviour.

| Document | The question it answered |
|---|---|
| [CAROUSEL-REPORT.md](CAROUSEL-REPORT.md) | Was the carousel with the MFSK floor ready to replace stop-and-wait? On-air and simulator results, what broke on the way, and the deep-fade collisions still open. |
| [HARQ-FINDINGS.md](HARQ-FINDINGS.md) | Does soft-combining repeated frames actually rescue a link at the fading cliff? (Yes — roughly half the frames, where single-shot delivers none.) |
| [FADE-CLIFF-DECISION.md](FADE-CLIFF-DECISION.md) | Should the S1 fade-cliff fix be merged? Simulation evidence behind the decision. |
| [SPEED-REGRESSION-FINDINGS.md](SPEED-REGRESSION-FINDINGS.md) | Why a pre-2.0 build was slower than v1.9.9 on the bench — FEC under fading and gear-shift oscillation, not the guard intervals everyone suspected. |
| [OTA-PHASE-A-SNR-CALIB.md](OTA-PHASE-A-SNR-CALIB.md) | Why the mode ladder stuck at the bottom: per-mode SNR estimation bias. |
| [broadcast-length-prefix-root-cause.md](broadcast-length-prefix-root-cause.md) | Why VarAC broadcasts would not decode. |
| [BROADCAST-NNCP-TRIAL.md](BROADCAST-NNCP-TRIAL.md) | A real NNCP bundle carried over the broadcast plane on the bench: where it stops working, and why the cliff is a carousel-length limit rather than a decode floor. |
| [MFSK-PORT.md](MFSK-PORT.md) | Porting the v1 32-MFSK fringe waveform to C, and how deep it actually reaches. |
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
