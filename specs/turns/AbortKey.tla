------------------------------ MODULE AbortKey --------------------------------
(***************************************************************************)
(* The callee's out-of-turn DISCONNECT (ABORT, abort_must_key in          *)
(* arq_fsm.c): no slot of the caller's came by the host's key-by time, so *)
(* S keys it on a clear channel -- not over a keydown of the caller's,    *)
(* which a half-duplex caller cannot hear it through, and it is not       *)
(* retried.  S takes the channel for clear when no decoder holds a sync   *)
(* (peer_is_transmitting) and the time is past channel_clear_at.          *)
(*                                                                         *)
(* Time 0 is the end of S's last keydown, which asked M for a round on a  *)
(* rung ("ofdm", any waveform a decoder syncs on, MFSK included; "psk",   *)
(* none: the PSK decoder finds a frame once it has ended, modem_psk.c) or *)
(* for nothing.  M answers it, if it heard it, within the reply window,   *)
(* Tg..Tg+W (the turn rules: specs/turns/CarouselTurns.tla), with that    *)
(* round or with control (sensed); or keys nothing.  A sensed keydown is  *)
(* seen Acq after it starts.  A round lasts at most R (s_round_ms of the  *)
(* rung and size asked for).  M does not hear S's out-of-turn keydown.    *)
(*                                                                         *)
(* channel_clear_at: the reply window and the acquisition (Tg+W+Acq),     *)
(* and, for a round asked for on a PSK rung, that round's length too      *)
(* (car_unsensed_round_ms).                                                *)
(*                                                                         *)
(* Safety: S's DISCONNECT never meets M's keydown (NoOverlap).            *)
(* Mutant "pskblind": no PSK term (#628 before review; the sim keyed 2.8 s *)
(* into a PSK3 round at -1 dB).                                            *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Tg, W, Acq, R, Ld, H, Mutant

Asks  == {"ofdm", "psk", "nothing"}

VARIABLES t, asked, mKind, mLen, mStart, sKey
vars == <<t, asked, mKind, mLen, mStart, sKey>>

None == H + 1                       \* not keyed (yet)

TypeOK == /\ t \in 0..H
          /\ asked \in Asks
          /\ mKind \in {"ofdm", "psk", "none"}
          /\ mLen \in 1..R
          /\ mStart \in 0..H \cup {None}
          /\ sKey \in 0..H \cup {None}

\* What M answers with: the round asked for, control, or nothing (it lost
\* the poll, or has nothing to say in time).
Answers(a) == IF a = "psk" THEN {"psk", "ofdm", "none"} ELSE {"ofdm", "none"}

Init == /\ t = 0
        /\ asked \in Asks
        /\ mKind \in Answers(asked)
        /\ mLen \in 1..R
        /\ mStart = None
        /\ sKey = None

Sensed == mStart # None /\ mKind = "ofdm" /\ mStart + Acq <= t /\ t < mStart + mLen

ClearAt == Tg + W + Acq + (IF asked = "psk" /\ Mutant # "pskblind" THEN R ELSE 0)

MKey == /\ mStart = None /\ mKind # "none" /\ t >= Tg /\ t <= Tg + W
        /\ mStart' = t
        /\ UNCHANGED <<t, asked, mKind, mLen, sKey>>

SKey == /\ sKey = None /\ ~Sensed /\ t >= ClearAt
        /\ sKey' = t
        /\ UNCHANGED <<t, asked, mKind, mLen, mStart>>

\* M keys within its window, if at all.
Tick == /\ t < H
        /\ ~(mStart = None /\ mKind # "none" /\ t = Tg + W)
        /\ t' = t + 1
        /\ UNCHANGED <<asked, mKind, mLen, mStart, sKey>>

Next == MKey \/ SKey \/ Tick
Spec == Init /\ [][Next]_vars

Meet(a, b, c, d) == a < d /\ c < b
NoOverlap == sKey = None \/ mStart = None \/ ~Meet(sKey, sKey + Ld, mStart, mStart + mLen)
=============================================================================
