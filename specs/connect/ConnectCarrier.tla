---------------------------- MODULE ConnectCarrier -----------------------------
(***************************************************************************)
(* The connect's carriers and timing, which ConnectHandshake leaves out:   *)
(* datalink_arq/arq_fsm.c (arq_call_carrier, call_answer_carrier,          *)
(* accept_carrier, call_must_wait, fsm_calling, fsm_accepting), in         *)
(* discrete time.                                                          *)
(*                                                                         *)
(*   C, the caller, keys CALL i on the control mode ("fast", DATAC16) for  *)
(*   i <= 2, then on the floor (MFSK) and fast in turn, each the call      *)
(*   interval of its carrier after the last one ended (IvFast, IvFloor).   *)
(*   From a repeat (i >= 2) on, or a floor CALL, it also listens on the    *)
(*   floor.  A CALL that comes due while one of C's decoders is on a     *)
(*   frame arriving waits (decoder_hears_frame: not the energy busy       *)
(*   detector); once it has waited, also until DW past that frame's end,  *)
(*   by when the frame has decoded.  R CALLs, then it gives up.           *)
(*                                                                         *)
(*   E, the callee, decodes a CALL up to D(carrier) after its end and     *)
(*   keys its ACCEPT TG later: on the CALL's carrier -- or on the floor    *)
(*   for a repeat on fast, unless its last ACCEPT went there.  E keys      *)
(*   nothing else: no blind ACCEPT.                                        *)
(*                                                                         *)
(*   C, decoding an ACCEPT (up to D after its end, on a carrier it listens *)
(*   on), is connected: it stops calling and keys its first round TG       *)
(*   later (LM long, the longest).                                         *)
(*                                                                         *)
(* Half duplex: a frame that overlaps the receiver's own keydown is lost. *)
(* The channel, per direction and carrier, delivers a frame always, never *)
(* or as it likes ("any"); C's decoder is on an ACCEPT ("senses" it) from *)
(* SA after it starts until it ends, always or as the channel likes       *)
(* (Sense: a preamble missed).                                             *)
(*                                                                         *)
(* Time unit 0.25 s.  Lengths and latencies round up (a later, longer     *)
(* ACCEPT is the harder case): DATAC16 3.73 s -> LF 15; MFSK 13.55 s ->   *)
(* LM 55; ARQ_CHANNEL_GUARD_MS 700 -> TG 3; a DATAC16 decode within 0.5 s *)
(* -> DF 2; an MFSK one within the floor's 4.5 s decode allowance -> DM   *)
(* 18; ARQ_CALL_DECODE_WAIT_MS 5000 -> DW 20; the call intervals, 8 s and *)
(* 13.5 + 10.5 s -> IvFast 32, IvFloor 96; a preamble caught 1.5 s in   *)
(* -> SA 6.  (At 0.5 s the rounding ate the floor CALL's 0.75 s of slack, *)
(* and its ACCEPT's decode tied with the caller giving up.)               *)
(* The hold's cap (a floor frame and DW) is not modelled: an ACCEPT ends.  *)
(*                                                                         *)
(* Properties:                                                            *)
(*   NoOverlap  (Sense "always") C and E are never on the air at once;    *)
(*   Early      (the fast forward path and the floor both ways delivering, *)
(*              the fast return dead, as on air 2026-10-21; Sense          *)
(*              "always") C connects on the answer to its second CALL: it  *)
(*              never keys a third;                                        *)
(*   Connects   (the floor both ways delivering, Sense "any") C is         *)
(*              connected by the horizon, whatever the fast paths do and   *)
(*              whether or not it senses an ACCEPT.                        *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS LF, LM, TG, DF, DM, DW, SA, IvFast, IvFloor, R, H,
          FwdFast, RetFast, FwdFloor, RetFloor,  \* "always", "never" or "any"
          Sense,  \* "always" or "any": is a decoder of C's on an ACCEPT arriving?
          Mutant  \* "none", or one rule broken (to show the model can fail):
                  \*   "old":       the code before: no floor answer to a
                  \*                repeat, no floor listening, no hold
                  \*   "nohold":    no hold: a CALL goes when it comes due
                  \*   "nodecwait": the hold ends with the frame (DW 0)
                  \*   "noalt":     E answers every repeat on fast on the floor
                  \*   "nolisten":  C does not listen on the floor after a repeat

VARIABLES t,
          cSt,      \* "calling", "connected", "gaveup"
          k,        \* CALLs keyed
          cNext,    \* when C keys its next CALL (-1: none planned)
          cHeld,    \* that CALL has waited (call_must_wait's defer_count > 0)
          cTx,      \* C's latest keydown: [s, e, call]  (call 0: the round)
          cListen,  \* C listens on the floor
          cDec,     \* when C decodes an ACCEPT (-1: none coming; -2: spent)
          eTx,      \* E's latest keydown, an ACCEPT: [s, e, car, sensed]
          eDec,     \* when E decodes a CALL (-1: none coming)
          eDecCar,  \* ...the carrier it came on
          eHeard    \* E has answered this session

vars == <<t, cSt, k, cNext, cHeld, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard>>

Len(car) == IF car = "fast" THEN LF ELSE LM
Dec(car) == IF car = "fast" THEN DF ELSE DM

\* arq_call_carrier: CALLs 1 and 2 fast, then the floor and fast in turn.
CallCar(i) == IF i <= 2 THEN "fast" ELSE IF (i - 3) % 2 = 0 THEN "floor" ELSE "fast"
Wait(i)    == IF CallCar(i) = "floor" THEN IvFloor ELSE IvFast
\* call_answer_carrier, as send_call_accept binds the payload decoder to it.
Listen(i)  == CallCar(i) = "floor" \/ (i >= 2 /\ Mutant \notin {"old", "nolisten"})

\* accept_carrier: E's carrier for an ACCEPT to a CALL on car.
AcceptCar(car) == IF car = "floor" THEN "floor"
                  ELSE IF eHeard /\ Mutant # "old" /\ (eTx.car # "floor" \/ Mutant = "noalt")
                       THEN "floor" ELSE "fast"

\* May a frame in this direction, on this carrier, arrive?
Path(dir, car) == IF dir = "fwd" THEN (IF car = "fast" THEN FwdFast ELSE FwdFloor)
                  ELSE (IF car = "fast" THEN RetFast ELSE RetFloor)
Choice(p) == IF p = "always" THEN {TRUE} ELSE IF p = "never" THEN {FALSE} ELSE BOOLEAN

On(x) == x.s <= t /\ t < x.e
Meet(x, y) == x.s < y.e /\ y.s < x.e

\* call_must_wait: a decoder of C's is on E's ACCEPT, or C has waited and it ended
\* less than DW ago.
Held == /\ Mutant \notin {"old", "nohold"}
        /\ eTx.sensed
        /\ \/ (eTx.s + SA <= t /\ t < eTx.e)
           \/ (cHeld /\ eTx.e <= t /\ t < eTx.e + (IF Mutant = "nodecwait" THEN 0 ELSE DW))

Init == /\ t = 0 /\ cSt = "calling" /\ k = 0 /\ cNext = 0 /\ cHeld = FALSE
        /\ cTx = [s |-> -1, e |-> -1, call |-> 0] /\ cListen = FALSE /\ cDec = -1
        /\ eTx = [s |-> -1, e |-> -1, car |-> "fast", sensed |-> FALSE]
        /\ eDec = -1 /\ eDecCar = "fast" /\ eHeard = FALSE

\* C's next CALL comes due while it must wait: a step later, then.
CHold == /\ cSt = "calling" /\ cNext = t /\ k < R /\ Held
         /\ cNext' = t + 1 /\ cHeld' = TRUE
         /\ UNCHANGED <<t, cSt, k, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard>>

\* C keys its next CALL.
CKey == /\ cSt = "calling" /\ cNext = t /\ k < R /\ ~Held
        /\ k' = k + 1 /\ cNext' = -1 /\ cHeld' = FALSE
        /\ cTx' = [s |-> t, e |-> t + Len(CallCar(k + 1)), call |-> k + 1]
        /\ cListen' = (cListen \/ Listen(k + 1))
        /\ UNCHANGED <<t, cSt, cDec, eTx, eDec, eDecCar, eHeard>>

\* Out of CALLs, the last one's wait over: C gives up.
CGiveUp == /\ cSt = "calling" /\ cNext = t /\ k = R
           /\ cSt' = "gaveup" /\ cNext' = -1
           /\ UNCHANGED <<t, k, cHeld, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard>>

\* A CALL ends: C plans the next; E may hear it.
CEnd == /\ cTx.call > 0 /\ cTx.e = t /\ cSt = "calling" /\ cNext = -1
        /\ cNext' = t + Wait(cTx.call)
        /\ LET car == CallCar(cTx.call) IN
           \E ok \in Choice(Path("fwd", car)), d \in 0 .. Dec(car) :
             IF ok /\ ~Meet(cTx, eTx) /\ eDec = -1
             THEN eDec' = t + d /\ eDecCar' = car
             ELSE UNCHANGED <<eDec, eDecCar>>
        /\ UNCHANGED <<t, cSt, k, cHeld, cTx, cListen, cDec, eTx, eHeard>>

\* E decodes the CALL and keys its ACCEPT TG later.
EAnswer == /\ eDec = t
           /\ \E sensed \in Choice(Sense) :
                LET car == AcceptCar(eDecCar) IN
                eTx' = [s |-> t + TG, e |-> t + TG + Len(car), car |-> car, sensed |-> sensed]
           /\ eHeard' = TRUE /\ eDec' = -1
           /\ UNCHANGED <<t, cSt, k, cNext, cHeld, cTx, cListen, cDec, eDecCar>>

\* An ACCEPT ends: C, still calling and listening on its carrier, may decode it.
EEnd == /\ eTx.e = t /\ cDec = -1
        /\ \E ok \in Choice(Path("ret", eTx.car)), d \in 0 .. Dec(eTx.car) :
             IF ok /\ cSt = "calling" /\ (eTx.car = "fast" \/ cListen) /\ ~Meet(cTx, eTx)
             THEN cDec' = t + d
             ELSE cDec' = -2       \* this ACCEPT is spent
        /\ UNCHANGED <<t, cSt, k, cNext, cHeld, cTx, cListen, eTx, eDec, eDecCar, eHeard>>

\* C decodes it: connected; its first round goes TG later (after the CALL it
\* may still be keying).
CConnect == /\ cDec = t /\ cSt = "calling"
            /\ cSt' = "connected" /\ cNext' = -1 /\ cDec' = -2
            /\ LET s0 == (IF cTx.call > 0 /\ On(cTx) THEN cTx.e ELSE t) + TG
               IN cTx' = [s |-> s0, e |-> s0 + LM, call |-> 0]
            /\ UNCHANGED <<t, k, cHeld, cListen, eTx, eDec, eDecCar, eHeard>>

\* Time passes when nothing is due now.
Due == \/ (cSt = "calling" /\ cNext = t)
       \/ (cTx.call > 0 /\ cTx.e = t /\ cSt = "calling" /\ cNext = -1)
       \/ eDec = t
       \/ (eTx.e = t /\ cDec = -1)
       \/ (cDec = t /\ cSt = "calling")
Tick == /\ ~Due /\ t < H
        /\ t' = t + 1
        \* a spent ACCEPT's mark clears once that ACCEPT is over
        /\ cDec' = (IF cDec = -2 /\ eTx.e <= t THEN -1 ELSE cDec)
        /\ UNCHANGED <<cSt, k, cNext, cHeld, cTx, cListen, eTx, eDec, eDecCar, eHeard>>

Next == CHold \/ CKey \/ CGiveUp \/ CEnd \/ EAnswer \/ EEnd \/ CConnect \/ Tick
Spec == Init /\ [][Next]_vars

\* ---- properties ----
TypeOK == /\ t \in 0 .. H /\ cSt \in {"calling", "connected", "gaveup"} /\ k \in 0 .. R
          /\ cHeld \in BOOLEAN /\ cListen \in BOOLEAN /\ eHeard \in BOOLEAN
          /\ eDecCar \in {"fast", "floor"} /\ eTx.car \in {"fast", "floor"}

NoOverlap == ~(On(cTx) /\ On(eTx))
Early     == k <= 2
Connects  == t = H => cSt = "connected"
=============================================================================
