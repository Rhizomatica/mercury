# The reverse path: what should carry an ACK?

An ARQ link has two directions and they are not symmetric in importance.  A
link that carries data forward but loses its ACKs stalls exactly as dead as one
that carries nothing, and this project has been bitten by that twice: once when
in-session ACKs rode a fragile short mode and a 5 kB transfer died around 2 kB,
and again when the ACK's airtime turned out to be the largest single component
of every turnaround.

So the reverse path deserves its own measurement rather than inheriting
whatever mode the forward path happens to be using.

This is that measurement.  It compares the three things that could carry an
in-session ACK, on one channel model and one SNR axis.

## What is being compared

| | airtime | payload | waveform |
|---|---|---|---|
| **pattern ACK** | 0.64 s | ACK / ACK+TURN / HAIL (~2 bits) | non-coherent Welch-Costas tone burst, detected by correlation |
| **DATAC14** | 0.69 s | 5 B | coherent OFDM + LDPC |
| **DATAC16** | 3.30 s | 16 B | coherent OFDM + LDPC |

DATAC14 is the interesting comparison and the reason it was added to
`acquire_vs_decode`: it is the only coded mode whose airtime is in the
pattern's class, so against it the pattern has to win on robustness alone, with
no airtime argument to fall back on.

## How it was measured

`utils/ackpat_chan_sweep` drives the shipped `mfsk_pattern_tx` /
`mfsk_pattern_detect` — not a reimplementation — through `utils/chanutil`, the
project's own Watterson channel, and reports the SNR3k the model measures from
the actual faded signal and added noise.  That is the same channel and the same
axis `acquire_vs_decode` uses for the DATAC modes, which is what makes the rows
below comparable at all.

This matters because the previous number for the pattern ACK — "10 to 12 dB
deeper than DATAC16" — came from `utils/ackpat_sweep`, which runs on flat AWGN
and reports a signal-RMS/noise-RMS ratio of its own.  That figure was not wrong
so much as not comparable, and it was optimistic: the real margin is smaller,
and the real argument for the pattern turns out not to be the margin anyway.

200 trials per point for the pattern and DATAC14, 100 for DATAC16.  Trials are
spaced by `chanutil_advance()` so a sweep of 0.64 s bursts samples as much of
the fading distribution as a sweep of multi-second ones.

## Result: the 50 % point

SNR3k at which half the ACKs get through:

| | AWGN | MPG | MPD |
|---|---|---|---|
| **pattern ACK** | **−13.9** | **−14.0** | **−13.2** |
| DATAC16 | — | −10.9 | −8.0 |
| DATAC14 | — | −9.8 | ≫ −9.6 |

The pattern is 3.1 dB deeper than DATAC16 on MPG and 5.2 dB deeper on MPD, at
one fifth of DATAC16's airtime.  Against DATAC14, at the *same* airtime, it is
4.2 dB deeper on MPG and far more than that on MPD, where DATAC14 delivered
17 of 200 at the top of the swept range and never recovered.

## The result that matters more: the ceiling

A 50 % point describes the cliff.  What a real link spends most of its time
near is the top of the curve, and there the two behave differently in kind:

| | reaches 100 % by | best seen in the sweep |
|---|---|---|
| **pattern ACK** | −1.7 dB (MPG), −4.4 dB (MPD) | 200/200, and stays there |
| DATAC16 | not within the swept range | 77/100 at −5.7 dB (MPG), 77/100 at −4.9 dB (MPD) |

Under fading DATAC16 does not converge to reliable delivery anywhere this
sweep reached.  It sits near 77 % on both MPG and MPD across the whole top of
its curve -- 79 % at −6.7 dB, 77 % at −5.7 dB -- because a burst that lands in
a fade is lost whatever the average SNR is.  The pattern converges: by −1.7 dB
on MPG it is 200 for 200 and stays there through +6.3 dB.

(A sweep further up, to +4 dB SNR3k, is still running; it will say whether
DATAC16's plateau eventually breaks or holds.  The comparison above stands on
what is measured either way, since the pattern is already saturated at the
SNRs where DATAC16 is at 77 %.)

One ACK in five lost on a *good* link is a retransmission on every fifth frame,
permanently, at the top of the ladder where throughput is supposed to be won.
That is a larger practical cost than the cliff position, and it is the
strongest single argument in this document.

## Channel sensitivity

| | AWGN | MPG | MPD | spread |
|---|---|---|---|---|
| pattern ACK | −13.9 | −14.0 | −13.2 | **0.8 dB** |
| DATAC16 | — | −10.9 | −8.0 | 2.9 dB |
| DATAC14 | — | −9.8 | ≫ −9.6 | collapses |

The pattern barely notices what the channel is doing.  Non-coherent envelope
detection has no phase to lose, so Doppler that pulls a coherent OFDM demod
apart costs it almost nothing.

This is the structural argument, and it is worth more than the dB figure: the
ACK *is* the reverse path, and the reverse path's conditions are precisely what
cannot be measured from the far end.  A sender choosing an ACK mode is choosing
blind.  A waveform whose performance does not depend on the thing you cannot
see is worth more than one that is better on average and occasionally much
worse.

## False accepts

Zero, in 6600 noise-only windows across all three channels and every SNR
point — and the ACK / ACK+TURN discrimination bit never flipped in 6600
trials.

A false accept is worse than a miss: the sender believes a frame landed when it
did not and moves on, leaving the receiver a hole.  Rule of three puts the
per-window rate below 0.05 % at 95 % confidence.  Note that the live detector
in `modem.c` slides its window every chunk, so the rate an operator sees is
this figure times the number of windows in the listen interval; the bound is
loose enough to hold, but it is a bound from zero observations, not a
measurement of a small number.

## What it costs

**Capacity, and this is the real constraint.**  The pattern channel carries
ACK, ACK+TURN and HAIL — about two bits.  It cannot carry a sequence number, a
selective-repeat bitmap, an SNR report or a mode request.

That is an exact fit for delivery-driven stop-and-wait, where only one frame is
outstanding so a heard ACK is unambiguous.  It is a hard stop for windowed ARQ,
whose selective repeat needs an epoch-tagged SACK bitmap.  The pattern ACK and
the windowed data plane genuinely pull against each other.  That is a design
decision to be taken deliberately, not a defect in either.

**CPU.**  The correlator was measured consuming 3.5k samp/s against 8k
arriving — roughly 44 % of the RX budget — which is why `expect_pattern_ack`
gates it to bounded windows rather than running it continuously.  Widening
those windows is not free on a Pi 4 already running two OFDM decoders.

## What the bench does not model, in the pattern's favour

The pattern burst measures a crest factor of 3.0 dB — near constant envelope.
DATAC OFDM runs 10 to 12 dB of PAPR.  A real transmitter is limited by peak
power, not average, so at the same PA setting the pattern gets several dB more
average power into the air than this bench gives it.

The on-air margin is therefore probably larger than the 3 to 5 dB measured
here.  That is a reason to check it on air, not a reason to assume it.

## Separability

The pattern generator and detector live entirely in `modem/mfsk.c`,
`modem/mfsk_sync.c` and `modem/mfsk_ofdm.c`.  Those three compile with zero
LDPC and zero freedv symbols — only the `modem_mfsk.c` backend wrapper drags in
the MFSK data modem.

So the pattern ACK does not depend on the MFSK data waveform and could carry
the reverse path for a DATAC-only ladder.  This is worth knowing because the
MFSK payload rung does *not* win: on MPG its 50 % point is near −7 dB against
DATAC16's −10.9 dB.  The two halves of the MFSK work can be adopted
separately, and they should be judged separately.

## What blocks dropping this into trunk's ARQ

Trunk runs `burst_frames = 1` for every mode in `arq_mode_table`, so its data
plane is stop-and-wait in practice.  That is the case the pattern fits: one
frame outstanding, so the coded ACK's `rx_ack_seq` is redundant and the
HAS_DATA flag it also carries is exactly what ACK+TURN encodes.  What the
pattern cannot carry is `snr_raw` -- which feeds OLLA's peer-SNR -- so a
pattern-only reverse path would blind link adaptation.  Sending the pattern for
the common case and a coded ACK periodically (or whenever the local SNR
estimate has moved) keeps OLLA fed and still takes the airtime and robustness
win on most frames.

The real obstacle is identity.  **The pattern carries no session ID**, and
`deliver_rx_checked()` drops any frame whose sequence is not exactly
`rx_expected` -- there is no NACK and no reordering buffer.  So an ISS that
accepts a foreign station's pattern advances its window past a frame the IRS
never got, and every subsequent frame is dropped: the transfer wedges and the
bytes are lost.  That is a worse failure than the retransmission a missed ACK
costs, and it is not hypothetical on a shared HF channel.

Giving the pattern an identity by rotating its tones per session works, but
only over half the rotation space.  Scoring every rotation of both tables
against every other at every alignment (exhaustive, integer arithmetic, no
channel involved):

| rotation difference | worst cross-score, of 16 |
|---|---|
| 8 and 24 | **8 — reaches the acceptance threshold** |
| every other difference | 6 or less |

The two colliding cases are structural, not bad luck.  The pattern is an
8-tone Costas array sent twice, and a shift of 8 symbols with a tone hop of 13
lands on 8·13 ≡ 8 (mod 32) — so a rotation by 8 aliases exactly onto the
unrotated pattern's second repetition.  The repetition that buys the pattern
its margin is what costs it half its identity space.

That leaves 16 mutually safe rotations (no two differing by 8 or 24), worst
cross-score 6 of 16 against a threshold of 8.  Four bits of session identity
with a two-symbol margin.  Whether two symbols is enough — noise can only add
matches to a foreign pattern, at roughly 1/32 per symbol — is the open
question, and raising the threshold to buy margin costs detection, which this
document's instrument can price.

Worth noting independently: the wedge above is reachable today, without any
pattern ACK, by a coded ACK that passes CRC with a corrupted sequence field.
Making the IRS re-ACK its actual `rx_expected` when it sees an unexpected
sequence would turn that wedge into a retransmission, and is worth doing on its
own merits.

## Reproducing

```
cd utils && make ackpat_chan_sweep acquire_vs_decode
./ackpat_chan_sweep  200  14 -22 2 mpg     # pattern, cliff through ceiling
./acquire_vs_decode  DATAC16 100 -34 -14 mpg
./acquire_vs_decode  DATAC14 200 -19  -8 mpg
```

`No` counts *down* from noisy to clean: the first numeric argument after the
trial count is the noisiest point.
