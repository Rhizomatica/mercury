---------------------------- MODULE ConnectCarrier -----------------------------
(***************************************************************************)
(* The connect's carriers and timing, which ConnectHandshake leaves out:   *)
(* datalink_arq/arq_fsm.c (arq_call_carrier, call_answer_carrier,          *)
(* accept_carrier, call_must_wait, fsm_calling, fsm_accepting), in         *)
(* discrete time.                                                          *)
(*                                                                         *)
(*   C, the caller, keys CALL i on the control mode ("fast", DATAC16) for  *)
(*   i <= F, then on the floor (MFSK) and fast in turn, each the call      *)
(*   interval of its carrier after the last one ended (IvFast, IvFloor).   *)
(*   F is ARQ_CALL_FAST_SLOTS: 2 by default, min(max(2, RETRIES/2), 4)     *)
(*   after a host's RETRIES (#442); the floor listening below keeps i >= 2.*)
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
(*              whether or not it senses an ACCEPT;                        *)
(*   FloorOnly  an end that transmits only MFSK keys nothing else.         *)
(*                                                                         *)
(* MfskTx: which end transmits only MFSK (arq:mfsk_tx_only, an FSK-only    *)
(* radio).  C then CALLs on the floor only, each the floor's interval      *)
(* after the last (the first too: arq_fsm.c sets its deadline from its     *)
(* carrier).  E then answers only where its ACCEPT goes on the floor       *)
(* (accept_keys): where it would go on fast it keys nothing, and the next  *)
(* CALL, a repeat C listens for on the floor, gets the floor ACCEPT.  A    *)
(* decoder of C's is on an ACCEPT only on a carrier C listens on.          *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS LF, LM, TG, DF, DM, DW, SA, IvFast, IvFloor, R, H,
          F,      \* fast CALLs before the floor (ARQ_CALL_FAST_SLOTS), >= 2
          FwdFast, RetFast, FwdFloor, RetFloor,  \* "always", "never" or "any"
          Sense,  \* "always" or "any": is a decoder of C's on an ACCEPT arriving?
          MfskTx, \* "none", "C", "E" or "CE": who transmits only MFSK
          Mutant  \* "none", or one rule broken (to show the model can fail):
                  \*   "old":       the code before: no floor answer to a
                  \*                repeat, no floor listening, no hold
                  \*   "nohold":    no hold: a CALL goes when it comes due
                  \*   "nodecwait": the hold ends with the frame (DW 0)
                  \*   "noalt":     E answers every repeat on fast on the floor
                  \*   "nolisten":  C does not listen on the floor after a repeat
                  \*   "loud":      an MFSK-only E answers a CALL on fast too,
                  \*                on the floor (#424 as opened, without its
                  \*                listener)
                  \*   "listenall": "loud", and C listens on the floor from its
                  \*                first CALL (#424 as opened: every station's
                  \*                MFSK listener on while calling)
                  \*   "firstiv":   an MFSK-only C's next CALL is due before its
                  \*                first ends (the first deadline set from the
                  \*                fast interval, as #424 as opened did)

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
          eHeard,   \* E has answered this session
          eLast     \* the carrier E chose for its last answer (call_rx_mode),
                    \* keyed or, from an MFSK-only E on fast, not

vars == <<t, cSt, k, cNext, cHeld, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard, eLast>>

CM == MfskTx \in {"C", "CE"}
EM == MfskTx \in {"E", "CE"}

Len(car) == IF car = "fast" THEN LF ELSE LM
Dec(car) == IF car = "fast" THEN DF ELSE DM

\* arq_call_carrier: CALLs 1 .. F fast, then the floor and fast in turn.
CallCar(i) == IF CM THEN "floor"
              ELSE IF i <= F THEN "fast" ELSE IF (i - F - 1) % 2 = 0 THEN "floor" ELSE "fast"
Wait(i)    == IF i = 1 /\ CM /\ Mutant = "firstiv" THEN 0
              ELSE IF CallCar(i) = "floor" THEN IvFloor ELSE IvFast
\* call_answer_carrier, as send_call_accept binds the payload decoder to it.
Listen(i)  == CallCar(i) = "floor" \/ (i >= 2 /\ Mutant \notin {"old", "nolisten"})
              \/ Mutant = "listenall"

\* accept_carrier: E's carrier for an ACCEPT to a CALL on car.
AcceptCar(car) == IF car = "floor" THEN "floor"
                  ELSE IF eHeard /\ Mutant # "old" /\ (eLast # "floor" \/ Mutant = "noalt")
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
        /\ eDec = -1 /\ eDecCar = "fast" /\ eHeard = FALSE /\ eLast = "fast"

\* C's next CALL comes due while it must wait: a step later, then.
CHold == /\ cSt = "calling" /\ cNext = t /\ k < R /\ Held
         /\ cNext' = t + 1 /\ cHeld' = TRUE
         /\ UNCHANGED <<t, cSt, k, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard, eLast>>

\* C keys its next CALL.
CKey == /\ cSt = "calling" /\ cNext = t /\ k < R /\ ~Held
        /\ k' = k + 1 /\ cNext' = -1 /\ cHeld' = FALSE
        /\ cTx' = [s |-> t, e |-> t + Len(CallCar(k + 1)), call |-> k + 1]
        /\ cListen' = (cListen \/ Listen(k + 1))
        /\ UNCHANGED <<t, cSt, cDec, eTx, eDec, eDecCar, eHeard, eLast>>

\* Out of CALLs, the last one's wait over: C gives up.
CGiveUp == /\ cSt = "calling" /\ cNext = t /\ k = R
           /\ cSt' = "gaveup" /\ cNext' = -1
           /\ UNCHANGED <<t, k, cHeld, cTx, cListen, cDec, eTx, eDec, eDecCar, eHeard, eLast>>

\* A CALL ends: C plans the next; E may hear it.
CEnd == /\ cTx.call > 0 /\ cTx.e = t /\ cSt = "calling" /\ cNext = -1
        /\ cNext' = t + Wait(cTx.call)
        /\ LET car == CallCar(cTx.call) IN
           \E ok \in Choice(Path("fwd", car)), d \in 0 .. Dec(car) :
             IF ok /\ ~Meet(cTx, eTx) /\ eDec = -1
             THEN eDec' = t + d /\ eDecCar' = car
             ELSE UNCHANGED <<eDec, eDecCar>>
        /\ UNCHANGED <<t, cSt, k, cHeld, cTx, cListen, cDec, eTx, eHeard, eLast>>

\* E decodes the CALL and keys its ACCEPT TG later -- or, transmitting only
\* MFSK, keys nothing where that ACCEPT would go on fast (accept_keys).
\* C's decoders are on it only on a carrier C listens on.
EAnswer == /\ eDec = t
           /\ LET car0 == AcceptCar(eDecCar)
                  car  == IF EM /\ Mutant \in {"loud", "listenall"} THEN "floor" ELSE car0 IN
              /\ IF EM /\ car = "fast"
                 THEN UNCHANGED eTx
                 ELSE \E sensed \in Choice(Sense) :
                        eTx' = [s |-> t + TG, e |-> t + TG + Len(car), car |-> car,
                                sensed |-> sensed /\ (car = "fast" \/ cListen)]
              /\ eLast' = car
           /\ eHeard' = TRUE /\ eDec' = -1
           /\ UNCHANGED <<t, cSt, k, cNext, cHeld, cTx, cListen, cDec, eDecCar>>

\* An ACCEPT ends: C, still calling and listening on its carrier, may decode it.
EEnd == /\ eTx.e = t /\ cDec = -1
        /\ \E ok \in Choice(Path("ret", eTx.car)), d \in 0 .. Dec(eTx.car) :
             IF ok /\ cSt = "calling" /\ (eTx.car = "fast" \/ cListen) /\ ~Meet(cTx, eTx)
             THEN cDec' = t + d
             ELSE cDec' = -2       \* this ACCEPT is spent
        /\ UNCHANGED <<t, cSt, k, cNext, cHeld, cTx, cListen, eTx, eDec, eDecCar, eHeard, eLast>>

\* C decodes it: connected; its first round goes TG later (after the CALL it
\* may still be keying).
CConnect == /\ cDec = t /\ cSt = "calling"
            /\ cSt' = "connected" /\ cNext' = -1 /\ cDec' = -2
            /\ LET s0 == (IF cTx.call > 0 /\ On(cTx) THEN cTx.e ELSE t) + TG
               IN cTx' = [s |-> s0, e |-> s0 + LM, call |-> 0]
            /\ UNCHANGED <<t, k, cHeld, cListen, eTx, eDec, eDecCar, eHeard, eLast>>

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
        /\ UNCHANGED <<cSt, k, cNext, cHeld, cTx, cListen, eTx, eDec, eDecCar, eHeard, eLast>>

Next == CHold \/ CKey \/ CGiveUp \/ CEnd \/ EAnswer \/ EEnd \/ CConnect \/ Tick
Spec == Init /\ [][Next]_vars

\* ---- properties ----
TypeOK == /\ t \in 0 .. H /\ cSt \in {"calling", "connected", "gaveup"} /\ k \in 0 .. R
          /\ cHeld \in BOOLEAN /\ cListen \in BOOLEAN /\ eHeard \in BOOLEAN
          /\ eDecCar \in {"fast", "floor"} /\ eTx.car \in {"fast", "floor"}
          /\ eLast \in {"fast", "floor"}

NoOverlap == ~(On(cTx) /\ On(eTx))
FloorOnly == /\ (EM /\ eTx.s >= 0 => eTx.car = "floor")
             /\ (CM /\ cTx.call > 0 => CallCar(cTx.call) = "floor")
Early     == k <= 2
Connects  == t = H => cSt = "connected"
=============================================================================
