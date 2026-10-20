------------------------------- MODULE PollRung -------------------------------
(***************************************************************************)
(* On which rung the callee's rounds go when the caller asks for them: the *)
(* other direction of RungAgreement.  Mirrors datalink_arq/carousel.c:    *)
(*                                                                         *)
(*   M, the caller, asks S for its round with a poll on the control mode  *)
(*   (m_timer's idle ask, m_decide_recv), which S decodes on any rung.    *)
(*   The poll names a rung, and M binds its payload decoder there as it   *)
(*   keys.  The rung is the one asked last (poll_level), or lower when    *)
(*   choose_level says so -- never higher on an ask -- and one lower      *)
(*   than a rung that has had M's asks and delivered nothing.             *)
(*                                                                         *)
(*   S, hearing the poll, answers in its slot with its round on the rung  *)
(*   named (m_on_ctl: tx_level from the poll's level).  S keys only to    *)
(*   answer a poll it heard.                                               *)
(*                                                                         *)
(* An exchange -- M's poll and S's answer -- is one step.  The channel    *)
(* loses any keydown, at most L in a row each way; a rung in Dead carries *)
(* nothing (a rung the connect's SNR put the session on, that the channel *)
(* does not support: sim, nvis, DATAC1).  The floor (0) is never dead.    *)
(*                                                                         *)
(* Not modelled: the MFSK floor's slots and patterns (FloorSilence), the  *)
(* lost-peer watchdog (240 s), rounds lost in part.                       *)
(*                                                                         *)
(* Properties:                                                            *)
(*   Agree      S's rounds go on the rung M listens on (invariant);       *)
(*   Delivered  every message of S's gets through (liveness).             *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS Rungs,   \* 0..n: the floor and the rungs above it
          N,       \* messages S sends
          L,       \* most keydowns the channel loses in a row
          Mutant   \* "none", or one rule broken (to show the model can fail):
                   \*   "noscore": every ask names the rung asked last (the
                   \*              code before 2026-10-18)
                   \*   "stale":   S answers on its own last rung, not the one named
ASSUME 0 \in Rungs /\ N \in Nat \ {0} /\ L \in Nat

VARIABLES
    dead,      \* the channel: rungs that carry nothing
    mLv,       \* M: the rung its next ask names (poll_level)
    mBind,     \* M: where its payload decoder listens
    delivered, \* M: rungs S's rounds have come on
    got,       \* M: S's messages it has
    sTx,       \* S: the rung of its rounds (tx_level)
    lossP,     \* the channel: M's polls lost in a row
    lossR,     \* ...and S's rounds on a live rung
    apart      \* ghost: S sent a round where M was not listening

vars == <<dead, mLv, mBind, delivered, got, sTx, lossP, lossR, apart>>

Lossy(lr) == IF lr >= L THEN {FALSE} ELSE BOOLEAN

Init ==
    /\ dead \in SUBSET (Rungs \ {0})
    /\ mLv \in Rungs /\ mBind = mLv            \* where the connect's SNR put it
    /\ sTx \in Rungs                           \* whatever S last had
    /\ delivered = {} /\ got = 0 /\ lossP = 0 /\ lossR = 0 /\ apart = FALSE

\* M asks on rung r; S answers if it heard.
Ask ==
    LET r == mLv IN
    /\ got < N
    /\ mBind' = r
    /\ \E lostP \in Lossy(lossP), lostR \in Lossy(lossR) :
        LET heard   == ~lostP
            tx      == IF Mutant = "stale" THEN sTx ELSE r
            live    == tx \notin dead
            decoded == heard /\ tx = r /\ live /\ ~lostR
            dlv     == IF decoded THEN delivered \cup {r} ELSE delivered
            \* the rung of the next ask, before choose_level may lower it
            next    == IF Mutant # "noscore" /\ r > 0 /\ r \notin dlv THEN r - 1 ELSE r
        IN
        /\ lossP' = IF lostP THEN lossP + 1 ELSE 0
        /\ lossR' = IF heard /\ live THEN (IF lostR THEN lossR + 1 ELSE 0) ELSE lossR
        /\ sTx' = IF heard THEN tx ELSE sTx
        /\ apart' = (apart \/ (heard /\ tx # r))
        /\ got' = IF decoded THEN got + 1 ELSE got
        /\ delivered' = dlv
        /\ mLv' \in 0 .. next
        /\ UNCHANGED dead

Done == got = N /\ UNCHANGED vars

Next == Ask \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(Ask)

\* ---- properties ----
TypeOK ==
    /\ dead \subseteq Rungs /\ 0 \notin dead
    /\ mLv \in Rungs /\ mBind \in Rungs /\ sTx \in Rungs
    /\ delivered \subseteq Rungs /\ got \in 0 .. N
    /\ lossP \in 0 .. L /\ lossR \in 0 .. L
Agree == ~apart
Delivered == <>(got = N)
=============================================================================
