# specs/rungs

Rung agreement: the caller's rounds go on the rung the callee's payload
decoder listens on, under any loss.  `docs/CAROUSEL-SPEC.md` §3.2 has the
wire fields (a poll's `level`, and the ack-only poll that names a rung, #386).

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-rungs -config RungAgreement.cfg RungAgreement.tla

Properties:

- `Agree`: whenever the caller has just heard the callee, both are on the
  same rung.
- `WastedF`: rounds sent to a rung the callee is not bound to, since the
  last loss (any keydown lost, the round part of a `KD_BOTH` alone, or an
  answer), at most `W`; a decoded round resets it.  The callee re-aligns
  within `W` rounds once the channel stops losing.  (It replaces `Wasted`,
  rounds in a row: once a round can be lost alone, separate losses add up
  waste the channel caused, and no protocol bounds that.  Resetting it at any
  round sent to the callee's rung is no fix: `prevr` and the `OkOnReq` rule
  would stop failing.)
- `FallbacksAgree`: while the callee's fallback is armed, the rung it goes
  back to (`s_prev_lv`) is the caller's (`ok_lv`).  `WastedF` cannot see
  `prevr`, which also wastes one round per loss.
- `Delivered`: every message gets through, and the caller knows it.  The
  channel loses any keydown, at most `L` in a row each way.  Under strong
  fairness, a loss-free exchange the callee can decode, possible again and
  again, eventually happens.

Results (`KD_BOTH` on any rung, and its round lost alone, are modelled):

- `RungAgreement.cfg`: the code's rules (`OkOnReq`).  `Agree`, `WastedF`
  with `W = 1`, and `Delivered` hold: no error, 3234 distinct states.  4
  rungs, 4 messages and `L = 3`: 17168 states, no error.  `WastedF_Zero.cfg`
  (`W = 0`) fails: the round sent blind after a loss, the fall-back to
  `ok_lv`, can still miss.
- `FallbacksAgree.cfg`: with `OkOnReq = FALSE` (`ok_lv` only from answers to
  rounds) the two fallbacks agree (1566 states, with `Agree`, `Delivered`).
  `FallbacksAgree_Code.cfg`, the code's `OkOnReq`: violated.  A poll
  answering a REQ sets `ok_lv` to the rung of a round the callee may have
  missed, and the fallbacks part.  That costs at most one round per loss
  (`WastedF` = 1 above), not rounds "again and again" as this file said
  before 2026-10-30.  In the sim, `OkOnReq = FALSE` cost more than it saved:
  on `nvis`, chat was 5 % faster and a UUCP-like mix 9 % slower over 60
  seeds.  The code keeps `OkOnReq`.
- `Alt_reqonly.cfg`: not a fault: after a silence only a REQ, never a blind
  round.  `WastedF` holds with `W = 0` (2370 states), at the cost of a
  control round trip (13.5 s on the floor): a latency trade-off for the sim,
  not a correctness fix.
- `Mut_*.cfg`: one rule broken each.
  - `noreq`: violates `WastedF`, and `Delivered` too with it dropped.
    Without the REQ, a mismatch is for ever.
  - `nobind`, `deaf`: violate `Agree`.
  - `norecvfb` (`Mut_norecvfb.cfg`, `W = 1`): the turn passing to the caller
    after a `KD_BOTH` keydown that drew nothing, in `m_decide_recv`, sends
    on the same rung with no fall-back: the code before #448.  Violates
    `WastedF` with `W = 1`; `Mut_norecvfb_W2.cfg` (`W = 2`) holds.  So #448
    takes the waste after a loss from at most 2 rounds to at most 1, under
    the code's own `OkOnReq`.  This is the 2026-10-27 K7EK-VE4KLM log: a
    DATAC4 round keyed at a peer back on MFSK.
    `Mut_norecvfb_Delivered.cfg` (`Agree`, `Delivered`): no error, 3714
    states -- the silence's fall-back recovers one round later, as on air
    (the data arrived, 30-40 s late).
  - The two rules as #386 was merged, both since fixed:
    - `unnamed`: violates `Agree`.  An ack that names nothing, repeating
      one that was lost, left the caller on a rung the callee had left.
    - `prevr` (`OkOnReq = FALSE`): violates `FallbacksAgree`.  Naming a
      rung left the callee's fallback (`s_prev_lv`) apart from the caller's
      (`ok_lv`).

`RungAgreement` has no floor rung, so `WastedF` does not bound what happens
when the fall-back (#448's, or the older one in `m_decide_send`) goes onto
the MFSK floor by mistake: `FloorFallback` below covers that (#529).
`test_car_explore oct27` (`cliff:-5` bidirectional, frames 0, 3 and 6 lost)
is the regression test for the case the fall-back gets right.

## FloorFallback

`RungAgreement` and `FloorSilence` combined (`FloorFallback.tla`): the
floor is one of the rungs, with `m_decide_send`'s floor rule (keep going
while `floor_silent` is within the cap), S's floor poll that moves M up
with `ok_lv` left at the floor, M's control onto MFSK after `FL` silent
floor rounds (`FLOOR_SILENT_LOSS`, `ctl_deaf_on_floor_silence`; S decodes a
REQ there only bound to the floor), and S's own floor fallback after `FS`
silent exchanges (`S_FALLBACK_MS`).

The case (#529, VE4KLM-K7EK 2026-11-08): S's floor poll moves M up to rung
`r` with `ok_lv` at the floor; S decodes M's round on `r` and confirms it
(`s_prev_lv = r`) in an answer that is lost.  M cannot tell that from its
round lost, falls back to the floor, and sends floor rounds S is not
listening for.  After `FL` of them its REQ goes on MFSK, which S on `r` does
not decode either: only S's own silence ends it.  The cap
(`FLOOR_SILENT_FB`, 1) makes M ask on DATAC16, which S decodes on `r`,
after one blind floor round.  `floor_silent` is cumulative (reset only when
M hears S), so the rule reads "after the fall-back, ask once the floor has
been silent `FB` times since M last heard S".

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-floorfb -config FloorFallback.cfg FloorFallback.tla

`WastedFB`: rounds sent where S does not listen while M's fall-back is
unconfirmed, since the last loss, at most `B`.

- `FloorFallback.cfg`: the code's rules (`FB = 1`, `FMax = 6`, `FL = 2`,
  `FS = 3`: S_FALLBACK_MS, 90 s, over a 24-31 s floor exchange).  `Agree`,
  `WastedF` (`W = 2`), `WastedFB` (`B = 2`) and `Delivered` hold: no error,
  3665 distinct states.  `FloorFallback_FS8.cfg` (`FS = 8`): `Agree`,
  `WastedFB` (`B = 2`) and `Delivered` hold, 5609 states.
- `Mut_fb_six.cfg`: the old cap (`FB = 6`, `FLOOR_SILENT_MAX`).  Violates
  `WastedFB` with `B = 2` (3 rounds at `FS = 3`); `Mut_fb_six_FS8.cfg`
  violates it with `B = 6` (7 rounds at `FS = 8`).  `Delivered` still holds
  with the old cap: it costs time, not delivery.
- The smallest `B` that holds, by `FS` (1 rung above the floor, `N = 2`,
  `L = 2`), is min(`FB` + 1, `FS`) up to `FMax` + 1:

  | `FS`   | 1 | 2 | 3 | 4 | 5 | 6 | 8 | 10 |
  |--------|---|---|---|---|---|---|---|----|
  | `FB = 0` | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
  | `FB = 1` | 1 | 2 | 2 | 2 | 2 | 2 | 2 | 2 |
  | `FB = 2` | 1 | 2 | 3 | 3 | 3 | 3 | 3 | 3 |
  | `FB = 6` | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 7 |

  With the old cap only S's silence bounds the waste; with 1 the bound
  does not depend on `FS`.  At the default timers (`FS` 3-4) the cap
  saves 1-2 floor rounds per such loss, 24-62 s, about what the
  field log shows (#529).
- Found by the model, not fixed here: `WastedF` with `FB = 1` grows with
  `FS` (2, 2, 3, 6 at `FS` = 2, 3, 4, 8) through another path.  S's sized
  poll moves the session down onto the floor and arms `CAR_T_W`; M's first
  floor round is lost, so S goes back up (`s_prev_lv`).  M came to the
  floor on S's word, not by a fall-back, so `fell_back` is clear and it
  keeps going for up to `FLOOR_SILENT_MAX` rounds before it falls back up
  to `ok_lv`.  S staying at a floor it asked for while it reads M below the
  control mode (`CAR_T_W`, #548) avoids this; the model lets the timer take
  S up either way.  Not seen in the sim yet.
- Not modelled: the channel going down (`FloorSilence` covers it), time,
  KD_BOTH on the floor, and patterns as a separate signal.

## FloorSilence

What keeps a session alive when the channel falls below every mode but the
MFSK floor (`FloorSilence.tla`, carousel.c's silence rule): the channel may
go down for good at any step, after which only floor keydowns decode; M takes
its control and rounds to the floor after `FM` silent exchanges
(`SILENT_MS`); S falls back to the floor after `FS` (`S_FALLBACK_MS`), once
a silence, and its round timer (CAR_T_W, which may fire before or after the
fallback) does not take a fallen-back S off the floor.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-floor -config FloorSilence.cfg FloorSilence.tla

- `FloorSilence.cfg`: the code's rules.  `Delivered` and `FloorHolds`
  hold: no error, 696 distinct states; also for every `FM`, `FS` in 1..3
  (436-1184 states).
- `arq:silence_fallback_s` (#442) sets `SILENT_MS`, 110..600 s, and
  `S_FALLBACK_MS` follows at 60 %.  The model counts both in exchanges, so
  the range has to map to whole exchanges:
  - **Low end.**  An exchange, as S sees it, is the gap between two keydowns
    of M's it decodes.  The longest on a working link is ~61 s: BW500 at the
    floor, S's answer a full floor round (27.6 s), then M's MFSK500 control
    (27.6 s), the guards (0.9 s, 0.7 s) and MFSK's decode (4.5 s).  Above the
    floor it is shorter (a 31.6 s round answered by a 4 s control).  So
    `S_FALLBACK_MS` >= 61 s, `SILENT_MS` >= 102 s: the minimum is 110 s
    (S 66 s), and both timers are at least one exchange (`FM`, `FS` >= 1).
    Below that S fell back between the exchanges of a link that was
    answering, which an exchange-counted model cannot express; the timed
    model (`specs/turns`) would be needed to say what that costs.
  - **High end.**  The shortest exchange is DATAC16 both ways: two 4 s
    control keydowns and the guards, ~9.7 s (on air ~10 s, #442).  600 s is
    then at most `FM` = 62 and `FS` (360 s) = 37.
  - Checked: `Delivered` and `FloorHolds` hold for **every** `FM` in 1..62
    and `FS` in 1..37 (2294 runs, 436-31415 distinct states).  The four
    mutants still fail at `FM` = 62, `FS` = 37 (`nosilence`, `nostay`,
    `noread`: `Delivered`; `lagread`: `FloorHolds`), so the large counts do
    not make the properties hold vacuously.
  - Every pair is checked, not only `FS` = 60 % of `FM`, so the check also
    covers two stations configured differently: M's `SILENT_MS` comes from
    its own ini, S's `S_FALLBACK_MS` from its own.
  - The round timer firing is a choice in `SDeaf` (`w`): it may take S
    back to `sPrev`, or leave it where it is.  So S staying at the floor it
    asked for while it reads M below the control mode (`car_on_time`,
    `CAR_T_W`, #548) is among the behaviours checked, and the result is
    unchanged: no error, 696 distinct states.  Only taking a fallen-back S
    off the floor is excluded (`Mut_fs_nostay`).
  - Not modelled: M's shortcut after two unanswered REQs
    (`car_on_tx_done`: `reqs >= 2 ? S_FALLBACK_MS : SILENT_MS`), which takes
    M to the floor after its own `S_FALLBACK_MS`.  It only moves M there
    earlier, a smaller `FM`, which is inside the checked range.
- `Mut_fs_nosilence.cfg`: M never takes the control mode to be out.
  Violates `Delivered`: down for good, M's DATAC16 control is never heard.
- `Mut_fs_nostay.cfg`: the round timer takes S off the floor it fell back
  to.  Violates `Delivered`: M at the floor, S on DATAC4 for ever (the
  explorer found it in the code: cliff:-5, a floor poll and two handovers
  lost).
- `Mut_fs_noread.cfg`: S, having read the channel down, still names a
  faster rung.  Violates `Delivered`.  The model has always had S name the
  floor once its reading says so; the code did not until 2026-10-17.  With
  every measured rung out, `choose_level` walked down one untried rung per
  lost exchange from the connect's rung and ignored the readings.  The
  ladder's height bounded that walk, where here it never ends; on air it
  took 3-4 min to reach the floor.  `fall_level` now starts the walk at the
  rung the latest reading would start a session on.
- `Mut_fs_lagread.cfg`: S's answer to M's floor control says it hears M
  above the control mode, and M's control goes back to DATAC16.  Violates
  `FloorHolds` (`Delivered` alone still holds: 696 states, no error; the
  floor rounds deliver, a silence later).  The code did this until #431:
  S judged its deafness by the smoothed SNR, so one -9 dB MFSK reading
  after 0 dB read -2.7, and a Winlink client's 74 bytes took 311 s in the
  sim.  An MFSK frame's reading now decides alone.

### M's lost-peer watchdog (#623)

With `LOST` > 0, M ends the session after `LOST` exchanges with nothing of
S's heard (`car_lost_at`, `ARQ_CAR_PEER_LOST_MS`), or, once its control is
on the floor, not before `FM + FS + 2` (`car_recover_by_ms`).  S may decode
M's last DATAC16 control before the move while its own answers are lost, and
then listens at the floor only `FS` exchanges after it.  M's next floor
exchange is the first one S can hear.  `LOST` = 0 is no watchdog, which is
how the checks above run (`LOST = 0  LT = 0`: 696 states, as before).

A watchdog ends any session whose losses outlast it, so with `L` alone no
finite mark keeps `Delivered`.  The question here is the race, not that.
`LT` > 0 bounds the losses in all, and allows them only while the channel
is up: the asymmetry that sets up the race (S hears M, M misses S), with a
clean floor after the drop.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-floor -config FloorSilence_Lost.cfg FloorSilence.tla

- `FloorSilence_Lost.cfg`: the defaults in exchanges of ~34 s (M keyed
  every ~34 s in silence in the sim, nvis): `FM` = 5 (150 s), `FS` = 3
  (90 s), `LOST` = 7 (240 s), `L` = `LT` = 5.  `Delivered` and `FloorHolds`
  hold: no error, 4831 distinct states.
- `Mut_fs_oldmark.cfg`: the mark at `LOST` alone, as before #623.  Violates
  `Delivered` (4759 states).  The trace is the one in the issue: S decodes
  M's control at the fourth silent exchange with its answers lost, M moves
  to the floor at the fifth, S falls back at the seventh, and M's watchdog
  ends the session there.
- Checked for `FM` in 1..6, `FS` in 1..4, `LOST` in `FM + 1` ..
  `FM + FS + 2`, `L` = `LT` = `FM`, `N` = 2 (108 runs, 76-5454 distinct
  states).  The code's mark holds in all 108.  The old mark violates
  `Delivered` in 60 of them: exactly those with `LOST` <= `FM + FS`.  At
  `FM` = `FS` = 3 (M's shortcut after two unanswered REQs, `S_FALLBACK_MS`)
  the old 240 s (`LOST` = 7) holds, as the sim found with data in flight.
- The code bounds the move with no state: the first keydown end at or after
  `heard_ms + SILENT_MS`, plus one floor exchange (in silence M keys at
  least that often).  The model's `FM + FS + 2` counts from the move M
  actually made, so it is the tighter of the two: the code's mark is at or
  past it.  The code uses `SILENT_MS` even with two asks unanswered.  `reqs`
  may reach 2 only after `heard_ms + S_FALLBACK_MS`, and the move then
  comes on the `SILENT_MS` branch (sim, fade:-5, seed 1147).
- What it costs: the time a caller takes to leave a vanished peer, every
  frame lost from a connected session, in seconds after the loss (sim,
  `script_bench dead`, 20 seeds, all equal):

  | session | before #623 | #623 |
  |---|---:|---:|
  | idle, 2300 Hz | 240 | 336 |
  | idle, BW500 | 259 | 402 |
  | idle, `silence_fallback_s` 200 | 240 | 424 |
  | data in flight, defaults | 237 | 331 |
  | data in flight, BW500, `silence_fallback_s` 230 | 276 | 532 |

  Meanwhile the caller polls an empty channel and does not take CALLs from
  other stations.
- Not modelled: the callee's own watchdog.  At BW500 it may fire with M's
  first 27 s floor poll on the air (sim, fade:-5, 3 of 3000).  That is the
  mirror case, left for a follow-up.

#431 also brought two rules the model already had: at the silence, M's
rounds go to the floor with its control (`MDeaf` sets `mRung`; the code
kept the old rung for one more lost round), and after two unanswered asks
M's silence is S's (`S_FALLBACK_MS`), so `FM` = `FS` as in
`FloorSilence.cfg`.

Which model covers the other 2.0 changes:

- the direct probe (`choose_level` asks for the rung the SNR would start
  on): `RungAgreement`'s sized poll names any rung (`\E l \in Rungs`), so
  agreement and delivery hold for whichever it picks;
- the SNR snap (two readings more than `SNR_GATE_DB` to one side of the
  smoothed SNR move it): it changes only the reading S judges rungs by, so
  `RungAgreement` covers the rung S then names (any rung).  It also moves
  when an end decides it hears its peer below the control mode
  (`ctl_deaf`).  The rung models fix the control frame's mode per
  configuration (`CarouselTurns.cfg`, `CarouselTurns_mfsk.cfg`); the
  decision itself, for the caller, is `specs/ctldeaf/CtlDeaf.tla`;
- no first try of a rung while the SNR is well below its threshold (half the gate)
  (`choose_level`'s one-rung-up probe): it only narrows the rungs S may
  name, which `RungAgreement` leaves free (any rung);
- control on the control mode once the channel has risen past the peer's
  "deaf" (`ctl_on_floor`, `peer_hears_me_db`): it changes only which mode
  a control frame goes on, and only from the floor to the control mode,
  which the peer always decodes.  No model has that choice (the `ctl_deaf`
  decision behind it is `specs/ctldeaf`);
- a pattern only for the rung asked: the floor's patterns are outside
  `RungAgreement`; the rule only narrows when S answers with one, which
  `FloorSilence` does not depend on;
- an unanswered ask scored, and the next ask one rung lower when the rung
  asked has delivered nothing (`m_timer`, `m_decide_recv`): `PollRung`;
- a rung dead after 2 unanswered asks, not 4, once two rounds in a row
  brought nothing of the peer (dark, #497): `PollRung` with `K = 2`
  (`PollRung_dark.cfg`), a rung killed mid-session;
- full rounds at once on a rung well above its threshold that has lost
  nothing (`level_earned`): it changes only how many frames a poll asks
  for, within the 1 .. `keydown_cap` that `poll_size` already gave, which
  no model constrains (rounds are atomic in `RungAgreement` and `PollRung`;
  `CarouselTurns` bounds keydowns by the cap).
- a short poll on the rung it already chose asks only for the frames the
  backlog takes (`short_rung`): it only lowers how many frames a poll asks
  for, never raises them, which no model constrains (as for `level_earned`).
- a round's overhead charged at the poll it really costs, the floor frame
  when the poll goes on the floor (`round_overhead`): it changes only the
  goodput S scores rungs by, so S's choice; `RungAgreement` lets S name any
  rung, and agreement and delivery hold for whichever it names.
- no direct probe while S's control goes on the floor (`choose_level`): it
  only narrows the rungs S names, which `RungAgreement` leaves free.
- a small message drops the loss margin on a rung delivering under 60 %
  (`short_rung`): it changes only which rung and how many frames a short
  poll asks for, which `RungAgreement` leaves free.
- a silence on an untried rung with room to spare in the SNR is scored
  when M's next keydown comes, not at S's fallback (`s_settle_probe`); a REQ
  about the round S had already answered says M never had the poll, and the
  rung stays untried.  The fallback itself (back to `s_prev_lv`) is as
  before, so `Agree`, `WastedF` and `FallbacksAgree` see no change; the rule
  moves only the goodput S scores rungs by, and so the rung it names next,
  which `RungAgreement` leaves free -- naming the same rung again after a
  lost poll included (`\E l \in Rungs`).  Rerun 2026-11-25:
  `RungAgreement.cfg` and `PollRung.cfg`, no error.
- the fallback's deadline allows one frame more than S asked for, as M's
  `round_n` may send: it changes only when S gives a round up, and the
  model's fallback is untimed; S keys nothing at it, so the turn models'
  no-overlap (`specs/turns`) is unaffected.
- before anything is measured, the start follows my reading upward
  (`choose_level`: the start rung of the lower of `snr_last` and `snr_ema`,
  never below the connect's), and an ack's short rung may go up to where
  the reading, less the gate, would start a session (`s_idle_rung`): both
  change only which rung is named, which `RungAgreement` leaves free.
  Rerun 2026-11-25: `RungAgreement.cfg` and `PollRung.cfg`, no error.
- a connect heard only on MFSK starts deaf to the control mode
  (`CAR_DEAF_HARD`, arq_fsm.c), and an MFSK reading does not clear that;
  a deaf S may then name, for one frame, the rung its ladder chose
  (`deaf_rung`); a decoded OFDM frame, or the peer's report that it hears
  mine, ends the deafness.  The probe is a sized poll naming a rung, which
  `RungAgreement` allows for any rung, so agreement and delivery hold; the
  deafness itself is the `ctl_deaf` decision of `specs/ctldeaf`.

## PollRung

The other direction: on which rung the callee's rounds go when the caller
asks for them (`PollRung.tla`).  Each poll names a rung, the caller binds
its decoder there as it keys, and the callee answers on the rung named.  The
rung is the one asked last, or lower (`choose_level`), and one lower than a
rung that has had the caller's asks and delivered nothing, or that has had
`K` unanswered scored asks in a row (`level_dead`).  Some rungs may carry
nothing at all, and the channel may kill a live rung mid-session (`Kill`, at
most `Kills` times), one that has delivered or not; the floor always
carries.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-pollrung -config PollRung.cfg PollRung.tla

- `PollRung.cfg`: the code's rules, `K = 4` (`DEAD_RUN`), up to two rungs
  killed mid-session.  `Agree` and `Delivered` hold: no error, 12487
  distinct states (4 rungs, 3 messages, `L = 2`).
- `PollRung_dark.cfg`: `K = 2`, the run near a rung's threshold and, since
  #497, once two rounds in a row brought nothing of the peer (dark).  `Agree`
  and `Delivered` hold: 8413 states.  Also checked: every `K` in 1..4 with
  `Kills` in 1..3, `L = 3`, and 5 rungs with 3 kills (up to 57379 states),
  all no error.  The model does not show what #497 gains, which is time
  (DATAC3 dead at the third dark round, not the fourth or later: the sim
  and the NW8L log); it shows that stepping down after a rung dies cannot
  stall, for any `K`.  The model had no mid-session death before #497: its
  dead rungs were fixed at `Init`, so a rung that once delivered was live
  for ever.
- `Mut_noscore.cfg`: every ask names the rung asked last, as the code did
  before 2026-10-18.  Violates `Delivered`: a session that starts on a rung
  the channel does not carry asks there for ever.  In the sim (nvis, the
  callee's 64-byte banner) 26 sessions of 60 asked for DATAC1 until the
  lost-peer watchdog ended them; with the rule, 1.
- `Mut_stale.cfg`: the callee answers on its own last rung, not the one
  named.  Violates `Agree`.
- `Mut_nodrop.cfg`: no `K` rule, so M may ask on a rung that once delivered
  for ever.  Violates `Delivered` with one kill: rung 1 delivers, dies, and
  M asks there again and again (NW8L, IONOS MPP +5 -> -5 dB, DATAC3).  With
  `Kills = 0` it holds (3597 states), so the kill is what the rule is for.

Give each TLC run its own `-metadir`.
