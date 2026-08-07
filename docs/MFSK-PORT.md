# MFSK (v1 cl_mfsk) ported to C — and MFSK-vs-FSK comparison

Follow-on to docs/LOW-SNR-FLOOR-PLAN.md (which found a non-coherent FSK
weak-signal mode is the real lever for the deep fringe, and codec2's 2-FSK
`FSK_LDPC` already out-performs DATAC15 there). This ports Mercury **v1's
32-MFSK** (which reached ~−13 dB Es/N0) from C++ to pure C, and measures how
much its higher modulation order M buys over 2/4-FSK.

## The port — `modem/mfsk.{c,h}`
Faithful pure-C port of v1's `cl_mfsk` (originally Fadi Jerji, C++):
non-coherent M-FSK over OFDM subcarriers. `class` → `struct mfsk_t` + functions;
`std::complex<double>` → a plain `{double re, im}` struct (zero-ambiguity FFT
interop); `std::isfinite` → C99 `isfinite`. All tone tables (preamble,
Welch-Costas ACK/BREAK, Sidelnikov NB, directed-HAIL FNV-1a suffix) carried over
verbatim. Compiles clean under `-std=gnu11 -Wall -Wextra`.

Validated by `tests/modem/test_mfsk.c` (in the unit suite):
- **mod→demod round-trip is lossless** with no noise for M=4/8/16/32
  (Gray-code + tone-hop consistency).
- soft-LLR sign tracks the sent bit; higher M is more robust at fixed Eb/N0.

## MFSK-vs-FSK: measured modulation-order gain
Non-coherent, uncoded **BER vs Eb/N0**, driving the ported C demod through
frequency-domain AWGN / interleaved Rayleigh (the OFDM subcarrier-SNR model the
demod assumes). Nc=50, 1 stream, 20 k symbols.

**AWGN:**

| Eb/N0 | M=2 | M=4 | M=8 | M=16 | M=32 |
|---|---|---|---|---|---|
| 8 dB | 0.021 | 0.0020 | 0.0002 | 0.0000 | 0.0000 |
| 6 dB | 0.068 | 0.016 | 0.0039 | 0.0013 | 0.0002 |
| 4 dB | 0.142 | 0.062 | 0.031 | 0.016 | 0.0089 |

**Interleaved Rayleigh (per-symbol, non-coherent):**

| Eb/N0 | M=2 | M=4 | M=8 | M=16 | M=32 |
|---|---|---|---|---|---|
| 16 dB | 0.023 | 0.014 | 0.014 | 0.011 | 0.010 |
| 12 dB | 0.054 | 0.034 | 0.032 | 0.027 | 0.025 |
| 8 dB | 0.118 | 0.080 | 0.070 | 0.063 | 0.060 |

**Reading it** (at ~1 % BER, a typical LDPC-input operating point):
- **AWGN:** M=32 needs ~4 dB vs M=2's ~10 dB → **~6 dB power-efficiency gain.**
- **Rayleigh:** the gain compresses to **~1.5–2 dB** — under fast fading the fade
  (not the modulation order) dominates, and non-coherent detection of a faded
  symbol suffers regardless of M.

## Conclusion
1. The big fringe win is **going non-coherent FSK at all** (already shown: even
   2-FSK beats coherent DATAC15 on MPP).
2. v1's **32-MFSK adds a real ~6 dB on AWGN / steady (calm-NVIS) channels**, but
   only ~1.5–2 dB under fast fading. Worth it where the channel is steady; less
   so in deep fast fading.
3. The C port makes v1's 32-MFSK available to slot into a real v2 weak-signal
   mode (ARQ ladder bottom rung) — the OFDM framing (bins↔time FFT), preamble
   sync, and LDPC glue remain to wire it up as an actual mode (follow-up).

## Acquisition — is the MFSK mode acquisition-limited? (the real question)

A weak-signal *modulation* is pointless if the receiver can't **acquire** the
burst at those SNRs — this is exactly the wall that caps DATAC15 (coherent OFDM
preamble detection collapses below ~−7 dB SNR3k; see docs/LOW-SNR-FLOOR-PLAN.md).
The MFSK mode is only worth wiring up if its acquisition floor sits *below* its
decode floor.

What we can say with confidence:
- **v1's MFSK does not use OFDM coherent acquisition.** It brings its own
  *non-coherent* preamble detector (`time_sync_mfsk_corr` in v1 `ofdm.cc`):
  per-symbol energy matched-filter correlation, non-coherently combined over the
  preamble symbols, with (per v1's notes) ~2000:1 noise discrimination vs ~4:1
  for a plain FFT-energy method. Non-coherent detection needs no phase/frequency
  lock, so it works well below the coherent-OFDM threshold.
- **End-to-end evidence already backs this:** the codec2 `FSK_LDPC` figures in
  docs/LOW-SNR-FLOOR-PLAN.md are *full-pipeline* (real acquisition + sync +
  decode through `ch`), and non-coherent FSK still delivered 90 % at −6.8 dB MPP
  vs DATAC15's 63 %. So a non-coherent-FSK acquisition path is empirically **not**
  the wall that OFDM acquisition is.

### End-to-end acquire+decode floor through `ch` (ch-calibrated SNR3k)

The ported core (mfsk.c + mfsk_ofdm.c + mfsk_sync.c) was wired into a real
passband pipeline — TX: framing → baseband → carrier-mix to real passband int16;
`ch` (AWGN / MPP fading, so SNR3k is measured exactly as for DATAC15); RX:
downmix + lowpass FIR → `mfsk_sync` acquisition → OFDM demod → `mfsk_demod`.
Config: Fs 8000, Nfft 256, Nc 50 (~1.5 kHz occupied), carrier 2000 Hz, CP 8 ms
(> the 2 ms MPP delay, so ISI-safe). 32-MFSK, **uncoded** (no LDPC yet —
conservative).

**AWGN** — acquires (metric) + uncoded BER:

| SNR3k | acquire | metric | uncoded BER |
|---|---|---|---|
| −8.8 | yes | 0.76 | 0.0000 |
| −10.8 | yes | 0.66 | 0.0000 |
| **−12.8** | yes | 0.53 | **0.0000** |
| −14.8 | no | 0.40 | — |

**MPP fading** (vs DATAC15 delivered/100 for reference):

| SNR3k | acquire | metric | uncoded BER | DATAC15 |
|---|---|---|---|---|
| −6.8 | yes | 0.77 | 0.0000 | 63 |
| −8.8 | yes | 0.71 | 0.01 | 47 |
| −10.8 | yes | 0.63 | 0.03 | 22 |

**Bottom line — the port is decisively not pointless, and acquisition is NOT the
bottleneck.** Measured through the same `ch`/SNR3k pipeline as DATAC15, v1's
non-coherent MFSK acquisition (`time_sync_mfsk_corr`) holds (metric 0.6–0.9) far
below DATAC15's ~−7 dB acquisition wall — to ~−13 dB SNR3k on AWGN (matching v1's
claim) and reliably past −11 dB on MPP fading, where DATAC15 delivers only 22 %.
And decode is essentially free there even *uncoded* (≤3 % BER); an LDPC on top
extends the floor further. The mode carries its own acquisition, so it is not
gated by the OFDM preamble wall.

Caveats: uncoded (LDPC pending — conservative); a chosen HF-reasonable config,
not v1's exact WB geometry; single noise realization per point (trend/margins
are large). The remaining integration work is wiring this as an actual ARQ-ladder
mode (LDPC + mode-pool + OLLA entry), not a question of whether it can acquire.

### Coded floor (rate-1/16 LDPC wired in)

v1's rate-1/16 LDPC (ROBUST_0's FEC, N=1600/K=100) is now ported
(`modem/mfsk_ldpc.{c,h}` — systematic IRA encoder + min-sum decoder; encoder
verified H·c=0, noiseless encode→decode lossless) and wired end-to-end: TX info
→ encode → 32-MFSK → passband → `ch` → RX acquire → demod → LDPC decode. 15
independent frames/point, delivered = fully-correct info frames:

| Channel | SNR3k | delivered | DATAC15 (ref) |
|---|---|---|---|
| AWGN | −12.8 | 11/15 (73 %) | — |
| MPP | −8.8 | 8/15 (53 %) | 47 |
| MPP | −10.8 | 7/15 (47 %) | 22 |
| MPP | −12.8 | 5/15 (33 %) | ~0 |

**Key result: `delivered == acquired` at every point** — once a frame acquires,
the rate-1/16 code *always* decodes it. So the coded mode is **acquisition-
limited**, not decode-limited: the LDPC removed the decode floor. Coded MFSK
delivers ~2–3 dB deeper than DATAC15 on fading (47 % at −10.8 dB vs DATAC15's
22 %, and still 33 % at −12.8 where DATAC15 is ~0).

**Design consequence:** the next robustness lever is **acquisition**, not the
code — a postamble (dual-ended acquisition) or a longer/repeated preamble would
lift the fading delivery directly (the ~50 % at −9 to −11 dB is preamble misses,
not decode failures). Bitrate at this config: ~8 bps info (rate 1/16, 25 sym/s,
5 bits/sym); v1's WB geometry gives ~14 bps. Frame = 100 info bits (~10 B) per
1600-bit codeword.

### Postamble (dual-ended acquisition) + full mode comparison

Since the coded mode is acquisition-limited, a **postamble** was added
(`mfsk_generate_postamble` + `mfsk_sync_build_postamble_template`): a second
known-tone sequence (distinct tones) after the payload, so RX can sync on the
preamble **or** the postamble. Coded MFSK, MPP fading, same rx file (valid A/B):

| SNR3k | preamble-only | dual-ended (postamble) |
|---|---|---|
| −8.8 | 12/15 (80 %) | **15/15 (100 %)** |
| −10.8 | 9/15 (60 %) | **13/15 (87 %)** |
| −12.8 | 6/15 (40 %) | **10/15 (67 %)** |

The postamble buys **~2 dB** of floor (a second, independently-faded chance to
acquire).

**Full mode comparison — delivered %, MPP fading, same `ch` pipeline:**

| SNR3k | MFSK dual (coded) | DATAC16 | DATAC15 | DATAC1 / DATAC3 |
|---|---|---|---|---|
| −8.8 | **100 %** | 63 % | 50 % | 0 % |
| −10.8 | **87 %** | 40 % | 33 % | 0 % |
| −12.8 | **67 %** | 20 % | 3 % | 0 % |
| −14.8 | **20 %** | — | — | 0 % |

The MFSK weak-signal mode extends the usable floor **~3–4 dB below** the most
robust OFDM modes (DATAC15/16), and DATAC1/DATAC3 fail entirely below −6.8 dB.
The trade is bitrate: ~8 bps (this config) vs DATAC15's 68 — a deep-fringe
bottom rung for when even DATAC15 can't get through.

Caveats: MFSK measured /15 vs OFDM /30 (percentages comparable); the two go
through separate but identical `ch`-MPP pipelines at the same No→SNR3k; MFSK is
this study's HF-reasonable config, not v1's exact WB geometry.

### LDPC rate ladder — rate is (almost) free; 1/16 is the wrong default

The full v1 rate ladder is now ported (`mfsk_ldpc_{1,2,3,5,8}_16`, all N=1600 so
**same airtime**; payload K grows with rate). Coded delivery, MPP fading,
dual-ended, per rate:

| SNR3k | 1/16 (12 B) | 5/16 (62 B) | 8/16=½ (100 B) |
|---|---|---|---|
| −10.8 | 13/15 | 13/15 | 13/15 |
| −12.8 | 10/15 | 10/15 | 10/15 |
| −13.8 | 6/15 | — | 6/15 |
| −15.8 | 2/15 | — | 2/15 |

**All rates deliver identically.** `delivered == acquired` for every rate, and
even rate 1/2 decodes everything that acquires down to the acquisition floor —
because the 32-MFSK demod (large FFT processing gain, non-coherent) hands the
LDPC very clean LLRs. So the code rate barely affects the floor; it only sets
the payload.

**Consequences:**
- **1/16 is the wrong default** — it carries 12.5 B/frame where 8/16 carries
  100 B at the *same* floor and airtime. And 1/16 (~10 B usable) can't even hold
  the 14-byte CONNECT frame; **8/16 (100 B) fits CONNECT + real payload.**
- Pick the **highest** rate the demod supports; here that's 8/16 (rate ½).
  Frame = 100 B / ~13 s ≈ **60 bps** — comparable to DATAC15's 68 bps but
  ~3–4 dB more robust. So the mode is not merely a tiny bottom rung; at rate ½
  it's a more-robust alternative at similar throughput.

**Caveat (important):** this "rate is free" result is under idealised sim
conditions — no carrier-frequency offset and no timing drift over the ~13 s
frame. On real HF a residual freq offset smears tones across FFT bins over a long
frame, which would favour shorter frames / lower rates; the practical rate
ceiling must be set OTA (Pedro). Ship a small ladder (e.g. 5/16 and 8/16) so OLLA
can adapt, rather than a single fixed rate.

Unit tests (`tests/modem/test_mfsk.c`, all in the suite): mod/demod round-trip,
modulation-order gain, OFDM framing round-trip, preamble acquisition, LDPC
encode/decode for **all five rates**, postamble (distinct tones + acquisition),
and pattern detection (planted ACK found, noise rejected, BREAK≠ACK).

## Control plane — pattern-ACK vs coded (DATAC16) ACK

The data plane above uses **coded MFSK frames** (rate ladder). But an ACK on a
coded frame is slow: an MFSK coded frame is ~13 s, and even a codec2 **DATAC16**
control frame is **3.74 s**. v1 instead signals fixed control events (ACK / BREAK
/ keepalive) with **Welch-Costas tone patterns** — non-coherent energy detection,
no LDPC, no coherent lock. Ported here as `mfsk_detect_pattern`
(`modem/mfsk_sync.c`, from v1 `cl_ofdm::detect_ack_pattern`): slide the buffer
and count pattern symbols whose expected hopped tone is the **peak bin** for every
stream; detection = matched-symbol count ≥ threshold (8/16 for the M=32 ACK).

An ACK burst is **16 symbols = 0.64 s** (matches v1's "~725 ms" claim), vs a
3.74 s DATAC16 frame — **5.8× less airtime per ACK.** And it survives far deeper.
Detection rate over 30 independent bursts through `ch`/Watterson (delivered/total):

| SNR3k | pattern-ACK (AWGN) | pattern-ACK (moderate) | pattern-ACK (poor) | DATAC16 ACK (moderate) |
|---|---|---|---|---|
| −3 | 30/30 | 30/30 | 30/30 | ~7/15 |
| −5 | 30/30 | 30/30 | 30/30 | 1/5 |
| −9 | 30/30 | 30/30 | 29/30 | 0/1 |
| −11 | 30/30 | 29/30 | 29/30 | 0/0 |
| −13 | 30/30 | 25/30 | 23/30 | 0/0 |
| −15 | 30/30 | 16/30 | 17/30 | 0/0 |
| −22 | 30/30 | — | — | 0/0 |

**Pattern-ACK holds 100 % to ~−9 dB on fading (to −22 dB on AWGN) and >75 % to
~−13 dB, where a DATAC16 ACK has been dead since ~−5 dB — roughly 10–12 dB more
fade margin, at a fraction of the airtime.** This is the FFT processing gain of
non-coherent per-tone detection: it needs neither phase lock nor LDPC.

**False-alarm gate (safety):** on pure noise (no burst planted) the detector
scored **0/30** false ACKs *and* 0/30 false BREAKs at every level down to −26 dB.
With M=32 the chance of 8 tone-peak coincidences is negligible, so lowering the
operating SNR does not manufacture spurious control events.

**Why this matters for the ARQ blocker:** the marginal-link failure Mercury keeps
hitting is *ACK survival on the reverse path* — a fragile-mode ACK dies while the
forward data still gets through, stalling the transfer. A pattern-ACK that
survives ~10 dB below DATAC16 removes exactly that bottleneck, and its short burst
keeps turnaround fast. Confirms v1's split: **coded MFSK frames for data +
CONNECT/CQ; Welch-Costas patterns for ACK/BREAK/keepalive.** (`detect_ack_pattern`
is now ported; wiring it into the ARQ FSM — pattern control below DATAC16, coded
MFSK data below DATAC15 — is the remaining integration step, OTA-gated.)

## Reproduce
- Round-trip / gain unit test: `cd tests && make test_mfsk && ./test_mfsk`.
- BER sweeps: the throwaway harness `mfsk_ber.c` (scratch) links `modem/mfsk.c`,
  adds freq-domain AWGN/Rayleigh at a target Eb/N0, and counts hard-decision bit
  errors; swept over M∈{2,4,8,16,32}.
- Pattern-ACK vs DATAC16 ACK: throwaway `mfsk_pat.c` + `ack_sweep.sh` (scratch)
  — passband ACK bursts → `ch`/`watterson_test` → `mfsk_detect_pattern`, vs a
  DATAC16 frame through the freedv raw tool, over an SNR3k grid (AWGN/moderate/
  poor). Includes the pure-noise false-alarm gate.

---

## The fringe floor was the sync accept threshold, not the code

`mfsk_sync_search` ended with a magic constant, `return (best_fine_metric <
0.5) ? -1 : best_fine`. The per-symbol statistic is
`|corr|²/(E_tmpl·E_rx) = SNR_sym/(1+SNR_sym)`, so a 0.5 gate demands
**SNR_sym ≥ 0 dB** — and that one number, not the LDPC, was the weak-signal
limit of the whole mode.

The proof is that the code does not matter: sweeping all five ported rates
(8/16 down to 1/16, a 16× difference in strength) gave **byte-identical** FER
curves. Instrumenting the search confirmed it — at SNR3k −3.1 dB it returns
`off=-1, metric=0.344`, payload never attempted.

Measured on AWGN over the real two-burst (~104k sample) window:

    noise-only MAX metric    0.037 - 0.049   (flat vs SNR — it is normalised)
    signal @ SNR3k  -5.1 dB  0.219
    signal @ SNR3k  -9.1 dB  0.091

The signal stood ~10× above the noise floor while the gate sat *above the
signal*. `MFSK_SYNC_ACCEPT` is now 0.08 (~1.6× the measured noise maximum,
0 false syncs in 600 noise-only searches):

| | before | after |
|---|---|---|
| AWGN 50% FER | −0.6 dB | **−11.2 dB** |
| Watterson 2 ms/1 Hz 50% FER | +5.0 dB | **−3.7 dB** |
| Watterson error floor | 0.25–0.30 | **gone** |

That floor was sync failing during fades, not the decoder. DATAC15 measures
−10.3 dB on the same harness, so the ladder floor finally sits below the rung
above it.

A lower threshold needs false anchors to be survivable, and they were not:
`modem_mfsk` cached the located anchor and marked it tried, so a false peak
whose payload failed CRC left the decoder blind until it slid out of the
window. A resident payload that fails CRC now releases the anchor and the
search restarts one symbol past it (safe: the search returns the *earliest*
peak above threshold, so nothing decodable hides before it).

### Interleaving

The codeword is scattered across the burst (Fisher-Yates, fixed xorshift seed,
integer-only so both ends derive the same table). 60 trials/point:

    Watterson 2ms/1Hz    -3.1 dB   FER 0.35 -> 0.20
    Watterson 2ms/0.2Hz  -4.4 dB   FER 0.58 -> 0.47
    AWGN                 unchanged (as expected)

~19% more ARQ throughput at the fringe for no airtime. The gain is moderate
because at 1 Hz a 13.5 s burst already spans ~26 fades; it helps most when one
fade covers a large fraction of the burst.

### Frequency search — the defect the whole test suite was blind to

There was **no frequency tolerance at all**. Measured on the pre-fix decoder:
12 Hz dial offset decoded 10/10, **16 Hz decoded 0/10** — half a subcarrier
(31.25 Hz spacing) and the mode is deaf. Every sensitivity number here had been
measured on a perfectly tuned simulator.

Acquisition now correlates against preamble templates pre-rotated to 13
hypotheses spanning ±3 subcarriers (±94 Hz) in **half-bin** steps. Tolerance:
±12 Hz → **±100 Hz**. Two non-obvious requirements:

- **Half-bin steps, not whole.** A whole-bin grid leaves the worst case exactly
  between two hypotheses; it recovered 31/62/94 Hz but left 16/47/110 Hz at 0/10.
- **Argmax, not first-past-the-gate.** At half-bin spacing a neighbour 0.5 bin
  off still clears the threshold, latches the wrong offset, and every frame then
  fails CRC. The tell was 94 Hz going 10/10 → 0/10 when the grid got *finer*.

Latched on a CRC pass and dropped on a CRC failure: +21% decode CPU with signal
present, none idle, AWGN cliff and 0/200 false alarms unchanged.

## Measured and deliberately NOT shipped

- **codec2's non-coherent LLR model** (log-I₀ of amplitude + `max*`, replacing
  an energy difference with a ±5 clamp): byte-identical FER over 30
  trials/point at both the old and new threshold, ~6% more decode time.
- **Goertzel tone detection.** Profiled: the whole decoder is **0.23% of real
  time** (0.18 s CPU per 78.7 s of audio; idle sync 0.002 s over 156 calls).
  The old "RX runs at half real time" was real but is fixed. Nothing to optimise.
- **Shorter frames.** Needs a shorter codeword (all five codes are N=1600), and
  the premise fails: FER vs Doppler at fixed SNR shows the burst *relies* on
  fade diversity — 2.6 fades/burst → FER 1.00, 26 fades → 0.17. Cutting the
  burst 3× takes 0.2 Hz from 5.2 to 1.7 fades, i.e. a dead link.
- **Lower code rates.** 9 dB of rate reduction (1/2 → 1/16) buys 2.1 dB; with
  acquisition fixed the limit is elsewhere. Lowering the tone count is the
  better trade at equal sensitivity, but it too loses under fading.

## Cross-check against modem73 RFDM (coherent QPSK OFDM + polar)

modem73's ROBUST family was measured through **our** Watterson with **our**
SNR3k, which removes the usual calibration ambiguity. Per bit:

    channel                mode                     50% FER   per bit @149bps
    AWGN                   our MFSK (60 bps)        -11.2 dB      -7.2 dB
    AWGN                   RFDM RDMN-150 (149 bps)   -4.5 dB      -4.5 dB
    Watterson 2ms/1Hz      our MFSK                  -4.5 dB      -0.5 dB
    Watterson 2ms/1Hz      RFDM RDMN-150              0.0 dB       0.0 dB

**We win on AWGN by 2.7 dB per bit and tie under fading.** Their pilots (25%)
and cyclic prefix (33%) are dead weight against white noise and earn their keep
in fading. Conclusion: no sensitivity case for porting RFDM — and the earlier
hypothesis that coherent, pilot-aided modulation is what closes our gap to
capacity is **not supported**; that estimate ignored those overheads.

Their robust modes also use *long* frames (RDMN-150 = 27.5 s, RDM-300 = 13.8 s
vs our 13.1 s), independently confirming the fade-diversity result above. Their
4.42 s MFSK is a backup mode, not the robust one.

Caveats: RFDM points are 5–12 trials against 20–60 for ours, one fading
profile, and faded cross-rate figures carry the ~1.5 dB two-path offset
documented in `watterson_model.md`.

## Directed patterns: the suffix only addresses if the threshold forces it

A Welch-Costas pattern plus a few session-derived suffix symbols is an
attractive replacement for a coded ACCEPT — it is a *correlation against an
expected sequence*, not a decode, so it escapes the energy-per-bit wall that
sinks every short coded control frame (`docs/MODES.md`, "Fourth attempt").
`mfsk_set_hail_target()` already builds exactly this shape: the 16-symbol
pattern followed by 4 symbols derived from an FNV-1a hash.

There is an arithmetic trap in it. The detector scores matched symbols over the
whole 20-symbol sequence against one threshold, and **16 of those 20 symbols
are the shared pattern**. So any threshold ≤ 16 can be satisfied without a
single suffix symbol matching, and the suffix addresses nobody.
`mfsk_set_hail_target()` sets `hail_detect_threshold = hail_match_threshold +
MFSK_HAIL_SUFFIX_LEN` = **12 of 20**, which is in exactly that régime.

Measured with `utils/hail_suffix_sweep` (AWGN, 100 trials, one wrong-session
key), transmitting session A's directed pattern and asking whether session B's
detector accepts it:

| SNR3k | detect (thr 12) | wrong-session (thr 12) | detect (thr 18) | wrong-session (thr 18) |
|---|---|---|---|---|
| −12 dB | 96/100 | **73/100** | 12/100 | 0/100 |
| −10 dB | 100/100 | **100/100** | 86/100 | 0/100 |
| −8 dB | 100/100 | **100/100** | 100/100 | 0/100 |

At the hail threshold a directed pattern is accepted by the wrong station
essentially always.  This is latent rather than shipping —
`mfsk_set_hail_target()` is not wired into any live path — but fix the
threshold before wiring it: it must exceed `ack_pattern_nsymb`, so that
`threshold − ack_pattern_nsymb` suffix symbols are *forced* to match.

Selectivity is not free.  Requiring ≥2 of 4 suffix symbols (threshold 18/20)
gives 0/100 wrong-session acceptance here, and a residual structural rate
against a *random* other session of C(4,2)(1/32)²(31/32)² + … ≈ **0.56 %**
(≈1 in 178); ≥3 of 4 (threshold 19) drops that to 0.012 % but costs
sensitivity.  The dB bought back by the threshold is the whole trade:

- plain 16-symbol pattern, threshold 8/16 — usable to about **−15 dB**, but
  addresses nobody.  Fine for the in-session connect confirm, which is what
  ships: the session already exists and the correlator window is bounded.
- directed 20-symbol pattern, threshold 18/20 — usable to about **−10.5 dB**,
  i.e. comparable to DATAC16's −9.5 dB, in **800 ms against 3740 ms**.

So a directed pattern is a credible ACCEPT: about a dB better than the coded
frame and ~2.9 s shorter.  Before wiring it, measure the false-accept rate over
*many* random session keys rather than the single pair used here — the binomial
above is an estimate, not a measurement.
