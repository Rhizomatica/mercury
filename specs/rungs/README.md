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

Give each TLC run its own `-metadir`.
