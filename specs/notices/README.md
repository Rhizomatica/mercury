# specs/notices

What a callee tells its host about an incoming call (`PENDING`,
`CANCELPENDING`, `CONNECTED`), as `datalink_arq/arq_fsm.c` and `arq.c` say
it:
- `fsm_listening`'s `RX_CALL` (`cb_notify_pending`);
- `fsm_accepting`'s ACCEPT-retry exhaustion;
- the ACCEPT fallback and its hold of the host (`ARQ_TD_HOLDING`,
  `ARQ_ACCEPT_FALLBACK_HOLD_MS`, `release_fallback_hold`);
- `car_connect_callee`;
- `pending_hold` and `arrival_hold`.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 2 \
        -metadir /tmp/tlc-notices -config CalleeNotices.cfg CalleeNotices.tla

What it models:

- The callee's states: `idle` (LISTENING), `accepting`, `fallback`
  (LISTENING after the ACCEPT retries ran out, still seeded for the caller),
  `connected`, and `off` (the host took the radio: LISTEN OFF or its own
  CONNECT).
- `held` is the code's hold (`teardown == ARQ_TD_HOLDING`).
- `inHold` is time: within `ARQ_ACCEPT_FALLBACK_HOLD_MS` (90 s) of our last
  ACCEPT, while a caller that took it may still come.
- `aPend` is `arrival_hold`'s own `PENDING`: a decoder locked on a frame
  while LISTENING, before anything has decoded (#527).
- The host's view is `hp`, a `PENDING` not yet ended by a `CANCELPENDING`
  or a `CONNECTED`. `PENDING` may repeat (`pending_hold`, every 20 s).
- What can happen:
  - the caller's CALL decoding;
  - a lock that ends with no CALL for us (another station's frame, noise,
    or a caller's poll that did not decode);
  - the retries running out, with an ACCEPT keyed or not, and within the
    hold's time of it or not;
  - the hold's end, then the fallback's end;
  - the caller's first frame (the connect);
  - the caller's DISCONNECT;
  - the host's DISCONNECT/ABORT, LISTEN OFF or CONNECT;
  - LISTEN ON again;
  - a session's end.
- Time is abstract: the two timers are actions, and the hold's always
  comes first.

Not modelled:

- The caller (specs/connect has the handshake). Its frame, its DISCONNECT
  and a new CALL may come whenever they could.
- LISTEN OFF's grace in ACCEPTING (`ARQ_TD_LISTEN_OFF`): here it is
  honoured at once.
- The ABORT hold after a session (specs/teardown, `HoldOwnSession`).

Properties:

- `NoStrayCancel`: no `CANCELPENDING` without a `PENDING` open, so each
  `PENDING` gets one end.
- `Settled`: the host is held exactly while the callee answers
  (ACCEPTING), holds the fallback, or an arrival lock lasts. No path out
  of any of these leaves a `PENDING` open behind it.
- `LateHeld`: in the fallback, within the hold's time, the host is held,
  so a late connect finds a scanning host still on the channel.
- `HeldInTime`: the code holds only in the fallback, within its time.

`CONNECTED` may come with no `PENDING` open. That happens with a late
connect after the hold, from a frame no decoder locked on first (the
500 Hz MFSK is not watched for arrivals). A host takes it as a connect
like any other.

The hold (2026-11-24, #657): on air (VE4KLM answering K7EK, 30 m,
2026-11-23 08:24 and 15:37) the caller took our last ACCEPT. We decoded
none of its polls in ACCEPTING, and its next poll connected from the
fallback, 70 s and 51 s after that ACCEPT. But `CANCELPENDING` had gone
when the retries ran out, 31 s and 12 s before the `CONNECTED`. JNOS
does not scan, so the connect survived; a scanning host would have hopped
in between.

Mutants (each `.cfg` gives the violated property):

- `Mut_early.cfg`: `CANCELPENDING` as the retries run out (before #657).
  `LateHeld` is violated: call, retries out, and the host is released
  within the hold's time.
- `Mut_noguard.cfg`: `arrival_hold` runs during the hold (#657 before
  77bcbd4c). A caller's poll that locks and does not decode (the very case
  the fallback is for) ends in a `CANCELPENDING` mid-hold, and `Settled` is
  violated in 5 states. Played on, the hold's own `CANCELPENDING` follows
  with none open.
- `Mut_silent.cfg`: the host's DISCONNECT leaves a held fallback without
  its `CANCELPENDING` (no `release_fallback_hold` on `APP_DISCONNECT`).
  `Settled` is violated.

`CalleeNotices.cfg`: no error, 8 distinct states.
