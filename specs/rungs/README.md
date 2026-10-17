# specs/rungs

Rung agreement: the caller's rounds go on the rung the callee's payload
decoder listens on, under any loss.  `docs/CAROUSEL-SPEC.md` §3.2 has the
wire fields (a poll's `level`, and the ack-only poll that names a rung, #386).

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-rungs -config RungAgreement.cfg RungAgreement.tla

Properties:

- `Agree`: whenever the caller has just heard the callee, both are on the
  same rung.
- `Wasted`: rounds sent in a row on a rung the callee is not bound to, at
  most `W`.
- `Delivered`: every message gets through, and the caller knows it.  The
  channel loses any keydown, at most `L` in a row each way.  Under strong
  fairness, a loss-free exchange the callee can decode, possible again and
  again, eventually happens.

Results:

- `RungAgreement.cfg`: the code's rules.  `Agree` and `Delivered` hold:
  no error, 1458 distinct states.  4 rungs, 4 messages and `L = 3` give
  5592 states, also no error.
- `Wasted_Code.cfg`: the code's `ok_lv` rule (`OkOnReq`).  A poll answering
  a REQ sets `ok_lv` to the rung of a round the callee may have missed.
  `Wasted` is violated: a lost round after it can go once more where the
  callee is not listening, again and again.
- `Wasted_Strict.cfg`: `ok_lv` taken only from answers to rounds.
  `Wasted` holds with `W = 1`, and fails with `W = 0`.  In the sim this rule
  cost more than it saved: on `nvis`, chat was 5 % faster and a UUCP-like
  mix 9 % slower over 60 seeds.  The code keeps `OkOnReq`.
- `Mut_*.cfg`: one rule broken each, all with `OkOnReq = FALSE`.
  - `noreq`: violates `Wasted`, and `Delivered` too with `Wasted` dropped.
    Without the REQ, a mismatch is for ever.
  - `nobind`, `deaf`: violate `Agree`.
  - The two rules as #386 was merged, both since fixed:
    - `unnamed`: violates `Agree`.  An ack that names nothing, repeating
      one that was lost, left the caller on a rung the callee had left.
    - `prevr`: violates `Wasted`.  Naming a rung left the callee's
      fallback (`s_prev_lv`) apart from the caller's (`ok_lv`).

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

- `FloorSilence.cfg`: the code's rules.  `Delivered` holds: no error, 696
  distinct states; also for every `FM`, `FS` in 1..3 (436-1184 states).
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

Which model covers the other 2.0 changes:

- the direct probe (`choose_level` asks for the rung the SNR would start
  on): `RungAgreement`'s sized poll names any rung (`\E l \in Rungs`), so
  agreement and delivery hold for whichever it picks;
- a pattern only for the rung asked: the floor's patterns are outside
  `RungAgreement`; the rule only narrows when S answers with one, which
  `FloorSilence` does not depend on.

Give each TLC run its own `-metadir`.
