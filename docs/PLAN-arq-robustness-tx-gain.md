# Mercury HF Modem — Improvements Based on Gary K7EK Review

## Context

A field operator (Gary, K7EK) tested Mercury on real HF for several weeks and gave three concrete pieces of feedback. The codebase confirms each is a real gap, not a misperception:

1. **Mercury gives up too easily on weak/noisy channels.** When SNR drops or fading hits, "data transfer stops and Mercury just sits there." A 10-retry hard cap then disconnects the link.
2. **Gear shifting is slow.** Long silent gaps occur while internal timers count down. Mode upgrades require 4 consecutive clean ACKs; a 15-second mode-hold timer locks Mercury into the slowest gear after one bad burst.
3. **TX audio output is too quiet.** No TX-gain control exists anywhere in the code. Operators are forced to crank Windows mixer to compensate, which then disrupts other modems sharing the rig.

Goal: make Mercury behave more like VARA-HF in adverse conditions (persist, not disconnect; adapt fast, not stall) and give operators a per-modem TX level control.

---

## What the code actually does today (verified)

### Robustness / gear shifting

- `datalink_arq/arq_protocol.h:193-241` — all timing/threshold constants are compile-time `#define`s. None are exposed in `mercury.ini`.
  - `ARQ_DATA_RETRY_SLOTS_DEFAULT = 10` → after 10 retries the FSM force-disconnects
    (`datalink_arq/arq_fsm.c:1216-1257`).
  - `ARQ_MODE_HOLD_AFTER_DOWNGRADE_S = 15` → after a retry-forced downgrade Mercury cannot upgrade for 15 s, even if the channel recovers.
  - `ARQ_LADDER_UP_SUCCESSES = 4` → 4 consecutive clean ACKs required to step the speed ladder up.
  - `ARQ_RETRY_DOWNGRADE_THRESHOLD = 2` → 2 consecutive retries force a payload-mode downgrade.
  - `ARQ_SNR_HYST_DB = 1.0`, `ARQ_MODE_SWITCH_HYST_COUNT = 1`.
  - `ARQ_KEEPALIVE_INTERVAL_S = 20`, `ARQ_KEEPALIVE_MISS_LIMIT = 5` → ~100 s before keepalive-driven disconnect.
- `datalink_arq/arq_fsm.c:279-305` (`record_tx_outcome`) and `:316-416` (`select_best_mode` + `maybe_upgrade_mode`) implement the asymmetric down-fast / up-slow ladder.
- Retry intervals (per-mode) live in `datalink_arq/arq_protocol.c:59-65`: DATAC4=13 s, DATAC3=10 s, DATAC1=12 s, DATAC13=7 s.
- A few of these are already runtime-tunable via TCP commands (`RETRIES`, `CALLINT` — see `datalink_arq/arq.c:915-930` and `arq_protocol.h:198-211`), but never persisted and never reachable from the GUI/INI.

### TX audio output

- `modem/modem.c:619, 634, 642` — only place where modulated samples land in the playback buffer:
  ```c
  tx_buffer[total_samples++] = (int32_t)mod_out_short[i] << 16;
  ```
  No gain stage, no per-mode level, no clip protection. (The `<< 16` is the correct int16→int32 mapping; the issue isn't dynamic-range waste — it's that FreeDV's OFDM modes deliberately output a low-crest-factor signal and there is no user-tunable boost.)
- `audioio/audioio.c:171-435` (`radio_playback_thread`) — upsamples 6:1 and writes to the soundcard with no scaling.
- No `gain`, `volume`, `level`, `tx_level` parameter exists in `mercury.ini.example`, in the `mercury_config` struct (`common/cfg_utils.h:46-63`), or in any GUI handler.

### Config / GUI plumbing (the easy part)

- INI parser: `common/cfg_utils.{h,c}`. Adding a key = struct field + `CFG_KEY_*` macro + read/write glue in `cfg_read` / `cfg_write`.
- WebSocket bridge: `gui_interface/ui_communication.c:76-168` already implements live `set_audio_config` and `set_radio_config` commands that update state, restart subsystems, and persist to INI. New runtime knobs slot in here.
- PWA front-end: `gui_interface/websocket/web/mercury.html` (and `docs/app/index.html`).
- TCP control plane: `data_interfaces/tcp_interfaces.c` already has the `RETRIES` / `CALLINT` precedent for parameter commands.

---

## Proposed improvements

All three reviewer concerns are in scope (A, B, C). Each item lists the concrete change and why; ordered by impact-per-effort within each group.

### A. Robustness: stop disconnecting when the channel is just slow

**A1. Replace the hard 10-retry-and-drop with persistent retry + absolute no-progress budget.** *(highest impact, becomes the only behavior)*
- Today, 10 DATA retries → `send_ctrl_frame(DISCONNECT)` (`arq_fsm.c:1249-1256`).
- Change: when the existing per-frame retry budget exhausts, **force-downgrade one mode step (DATAC1→DATAC3→DATAC4), reset the retry counter, and keep going**. Only disconnect when (i) we are already in DATAC4 *and* the absolute no-progress wall-clock budget elapses, or (ii) keepalives miss `keepalive_miss_limit` in a row.
- New INI key `no_progress_timeout_s` (default 180 s = 3 min) caps the total time without forward progress.  Sits just above the keepalive timeout (5 × 20 = 100 s) so keepalive remains the normal disconnect path; this is a safety net for the asymmetric case where peer keepalives still arrive but our TX direction has gone one-way.
- The old "drop after N retries" path is removed outright — pre-2.0, no compat shim. Matches VARA-HF "keep trying" behavior unconditionally.

**A2. Make all retry / timeout / hysteresis values INI-tunable.**
- Promote the constants in `arq_protocol.h:193-241` to fields of `mercury_config`, with the existing `#define`s as defaults. Add `[arq]` section to `mercury.ini.example`.
- Most useful keys: `data_retry_slots`, `mode_hold_after_downgrade_s`, `ladder_up_successes`, `retry_downgrade_threshold`, `snr_hyst_db`, `keepalive_interval_s`, `keepalive_miss_limit`.
- Existing `arq_call_retry_slots` / `arq_callint_override_s` atomics (already runtime-mutable) become the model — extend the same pattern to the rest.

**A3. Shorten silent gaps between retries when the link is borderline.**
- `ARQ_CHANNEL_GUARD_MS=700` and `ARQ_ISS_POST_ACK_GUARD_MS=900` are tuned for one specific TX/RX switching latency. Expose them as INI keys (`channel_guard_ms`, `post_ack_guard_ms`) so operators with faster radios can tighten the gap and reduce idle-air time. Keep current values as defaults.

### B. Faster gear shifting

**B1. Lower the post-downgrade mode-hold timer.**
- `ARQ_MODE_HOLD_AFTER_DOWNGRADE_S = 15` is the single biggest source of "stuck in slow gear" behavior.
- Lower the default to 6 s and expose via INI (item A2).
- Stretch: make it adaptive — hold for `min(default, last_retry_burst_length × 2)` so isolated fades don't cost the full window.

**B2. Loosen the up-step gate.**
- `ARQ_LADDER_UP_SUCCESSES = 4` is conservative. With the existing SNR hysteresis already in place, 2 clean ACKs is usually enough signal.
- Lower default to 2 (still INI-tunable). The asymmetric "down on 1 retry / up on N successes" stays, just with smaller N.

**B3. Decouple SNR-driven upgrade from delivery-feedback ladder.**
- `select_best_mode` already supports SNR-only upgrades — but `maybe_upgrade_mode` won't act unless `tx_success_count` is also high enough, because of how the speed_level ladder gates it. When peer SNR is solidly above threshold + hysteresis, allow the upgrade regardless of the success counter. (Code change in `arq_fsm.c:316-416`.)

### C. TX audio gain control

**C1. Add a TX gain stage in the modem TX path.**
- New global `extern _Atomic float g_tx_gain`; default 1.0 (no change vs. today).
- Apply at `modem/modem.c:619, 634, 642` with saturation:
  ```c
  int64_t v = (int64_t)((int32_t)mod_out_short[i] << 16) * g_tx_gain_q16 / 65536;
  if (v >  INT32_MAX) v =  INT32_MAX;
  if (v <  INT32_MIN) v =  INT32_MIN;
  tx_buffer[total_samples++] = (int32_t)v;
  ```
  Use Q16 fixed-point or a `float` multiply guarded by `fminf/fmaxf` — pick whichever fits the surrounding style.

**C2. Expose `tx_gain_db` in `mercury.ini`.**
- Range: -20.0 … +20.0 dB, default 0.0 (= linear 1.0). Stored as float.
- Add `CFG_KEY_TX_GAIN_DB`, struct field `float tx_gain_db`, read/write in `cfg_utils.c`. Convert dB → linear at modem startup.

**C3. Add a TX-gain slider to the PWA.**
- New WebSocket command `set_tx_gain` in `ui_communication.c` (mirror the `set_audio_config` pattern at `:85-127`): updates the atomic, persists to INI via `cfg_write`. Slider in `gui_interface/websocket/web/mercury.html` and `docs/app/index.html`, range -20 to +20 dB, step 0.5 dB. Live update, no restart needed.

**C4. Add a TX peak/level indicator in the PWA.**
- Track instantaneous peak of `tx_buffer` per burst (post-gain, pre-clip and pre-saturation) in `modem/modem.c` after the loops at `:617-620`, `:632-635`, `:640-643`.
- Surface the peak through the existing waterfall/status WebSocket message stream (`gui_interface/ui_communication.c`) — add a `tx_peak_dbfs` field next to the existing waterfall payload so we don't add a new socket frame type.
- PWA: render a horizontal bar meter above the gain slider with a held-peak hairline that decays over ~2 s. Color: green < -6 dBFS, yellow -6 to -1 dBFS, red ≥ -1 dBFS so operators can set gain just below clip without an external SDR.

---

## Critical files

| File | Why |
|---|---|
| `datalink_arq/arq_protocol.h` | Promote `#define`s to runtime/INI-tunable values |
| `datalink_arq/arq.c` | Atomic vars, TCP-command precedent (`RETRIES`, `CALLINT`) |
| `datalink_arq/arq_fsm.c` | Retry-exhaustion path (`:1216-1257`), mode-hold logic (`:316-416`), ladder (`:279-305`) |
| `datalink_arq/arq_protocol.c` | Per-mode retry intervals (`:59-65`) if A3 is in scope |
| `modem/modem.c` | TX gain insertion point (`:619, 634, 642`) |
| `common/cfg_utils.{h,c}` | Add struct fields + INI keys for all new knobs |
| `mercury.ini.example` | Document new keys |
| `gui_interface/ui_communication.c` | New `set_tx_gain` and (optionally) `set_arq_tuning` WebSocket commands |
| `gui_interface/websocket/web/mercury.html` | TX gain slider + (optional) ARQ-tuning panel |
| `docs/app/index.html` | Same UI additions for the docs PWA |

---

## Verification

The hard part — these changes affect on-air behavior and must be tested against a real radio link.

1. **Bench (no radio):**
   - Run `make` (Makefile at repo root) and confirm no regressions.
   - Existing test harness in `tests/` — run the ARQ round-trip tests; new INI keys should default to current `#define`s, so all tests must still pass unchanged.
   - With two Mercury instances looped through `audioio`'s SHM mode (`sound_system = shm`), verify a clean transfer with default config matches today's behavior.

2. **TX gain (loopback):**
   - Use the existing `tap-capture.f64` / `tap-playback.f64` flow (see repo root) to record TX audio at gain = 0 dB and gain = +6 dB. Verify the second is exactly 2× peak amplitude with no clipping/wrap, and that gain = +20 dB clips cleanly without wrap-around (saturation works).
   - The peak meter readout from C4 should match the offline-measured peak within ±0.5 dB.

3. **GUI:**
   - With `ui_enabled = true`, open the PWA, drag the TX gain slider, watch a TX burst on an external SDR or scope; level should change live without a restart. Confirm the new value is written back to `mercury.ini`.
   - Confirm the peak meter tracks live, hold-and-decay works, and color zones flip at -6 dBFS / -1 dBFS thresholds.

4. **On-air robustness (the real test) — São Roque ↔ Belo Horizonte field link:**
   - Reproduce Gary's scenario on a marginal channel where today Mercury disconnects mid-transfer.
   - Confirm the link force-downgrades through DATAC3→DATAC4 and stays connected, instead of dropping after 10 retries.
   - Confirm `no_progress_timeout_s` *does* eventually drop the link when the channel is truly dead (e.g. unplug peer's antenna for >10 min).
   - Tune `mode_hold_after_downgrade_s` from INI; observe shorter idle gaps and faster recovery after fades.
   - Compare end-to-end transfer time and disconnect rate vs. today on the same channel.

5. **Regression on a clean channel:**
   - On a high-SNR link, confirm that with default config the throughput is no worse than today (i.e. the looser up-step gate doesn't cause harmful oscillation).
