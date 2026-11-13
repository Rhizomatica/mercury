# specs/ctldeaf

When the caller (M) takes itself deaf to the control mode, and when it
stops: `car_t.ctl_deaf` in `datalink_arq/carousel.c` (`car_on_frame`,
`ctl_deaf_on_asks`, `ctl_loss_expired`).  Deaf, M reports it in every frame
it sends, and the callee's control goes on the MFSK floor.
`docs/CAROUSEL-SPEC.md` (the `ctl_deaf` row) has the rules in words.

    java -cp tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-ctldeaf -config CtlDeaf.cfg CtlDeaf.tla

The smoothed reading is abstracted to four bands against `CTL_DEAF_ON_DB`
(-7.5), `CTL_DEAF_OFF_DB` (-6.0) and `CTL_LOSS_OFF_DB` (-3.0), and any frame
may read any band.  The states are `none`, `snr` (the reading's hysteresis),
`hard` (connect heard only on MFSK: only an OFDM frame clears it), `mfsktx`
(for good) and `loss` (#538: two asks unanswered while the callee is heard
and the reading is below -3.0 dB).  Time passes under weak fairness; nothing
else is owed.

Properties:

- `LossEnds`: LOSS, once set, ends, even if the reading stays in the
  hysteresis band or just above it and every ask goes unanswered.  It may be
  set again; each spell ends.
- `LossNeedsAsks`: LOSS is set only on two asks unanswered, with the callee
  heard and the reading below -3.0 dB.
- `HardNeedsOfdm`: HARD ends only on an OFDM frame.
- `MfskTxStays`: MFSK_TX is never left.

Not modelled: what the callee does with the report (it only picks the mode
of its control frames), the reading's arithmetic, the rungs (`specs/rungs`),
the silence rule (`specs/rungs/FloorSilence.tla`).

Results (`Hold = 2` ticks; `Hold = 5`: 740 states, no error):

- `CtlDeaf.cfg`, the code: no error, 524 distinct states.
- `Mut_nohold.cfg`, LOSS held until a reading of -3.0 dB or more (#538
  before review): `LossEnds` violated.  The counterexample is the sim's
  BW500 `cliff:-5` stall: LOSS set, the reading stays in the hysteresis band,
  asks keep going unanswered, and LOSS never ends.
- `Mut_fellback.cfg`, an unanswered round counted as a missed answer (#538
  before review): `LossNeedsAsks` violated, LOSS set after one ask.  A round
  lost on its way to the callee says nothing about the callee's control.
