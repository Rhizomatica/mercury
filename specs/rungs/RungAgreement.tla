---------------------------- MODULE RungAgreement -----------------------------
(***************************************************************************)
(* On which rung the caller's rounds go, and on which the callee listens:  *)
(* the two must agree, or the callee's payload decoder never syncs on the  *)
(* caller's frames.  Mirrors datalink_arq/carousel.c:                      *)
(*                                                                         *)
(*   M, the caller, sends each round on tx_level (mTx).  It changes it    *)
(*   only on S's word -- a sized poll names (lv, n), and an ack-only poll  *)
(*   may name the rung S listens on next (#386, s_idle_rung) -- and once  *)
(*   by itself: a round with no answer, on a rung other than ok_lv, is     *)
(*   sent again on ok_lv (m_decide_send, fell_back).  Every poll M hears  *)
(*   sets ok_lv to the rung of its last round (m_on_ctl) -- OkOnReq; with *)
(*   it FALSE, only one answering a round does -- and an ack naming a     *)
(*   rung sets it to that one.  After that, silence means a REQ, on the   *)
(*   control mode, which S always decodes.                                *)
(*                                                                         *)
(*   M's keydown may carry a poll with the round (KD_BOTH: the round on a *)
(*   rung M has no answer on yet, a probe).  S, decoding the poll but not *)
(*   the round, answers with a sized poll.  If that answer is lost, M's   *)
(*   next decision is made in the receive phase, where the turn passes to *)
(*   M (m_decide_recv, "turn: mine"): it too goes back to ok_lv once, if  *)
(*   nothing was heard and the round was on another rung               *)
(*   (fall_back_unconfirmed, #448).                                       *)
(*                                                                         *)
(*   S, the callee, binds its decoder where it says it will listen: the   *)
(*   rung of its sized poll, or the rung its ack names (send_poll_ex).    *)
(*   An ack that names no new rung names the one S listens on now; one    *)
(*   that does makes it the rung M's rounds come on (s_prev_lv).          *)
(*   A sized poll for a rung M's rounds never came on arms a fallback     *)
(*   (CAR_T_W): with no M keydown decoded by the end of the round, S goes *)
(*   back to the rung they last came on (s_prev_lv).  A REQ about a round *)
(*   S has answers with an ack; about one it missed, with a sized poll.   *)
(*                                                                         *)
(* An exchange -- M's keydown and S's answer in its slot -- is one step:  *)
(* the turn rules (specs/turns) keep them apart in time.  A round carries *)
(* one message whole.  The channel loses any keydown, at most L in a row *)
(* each way (a link dead one way is the lost-peer watchdog's: 240 s); a  *)
(* round on a rung S is not bound to is never decoded.                    *)
(*                                                                         *)
(* Not modelled: the MFSK floor (patterns, S_FALLBACK_MS), REQs after     *)
(* which M gives up (the lost-peer watchdog ends the session), and rounds *)
(* lost in part.                                                          *)
(*                                                                         *)
(* Properties:                                                            *)
(*   Delivered  every message gets through, and M knows it (liveness);    *)
(*   Agree      whenever M has just heard S, they are on the same rung;   *)
(*   Wasted     rounds sent where S does not listen, in a row, at most W. *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS Rungs,   \* the rungs above the floor (1..3 is enough)
          N,       \* messages M sends
          L,       \* most keydowns the channel loses in a row
          W,       \* the bound Wasted checks
          OkOnReq, \* TRUE: a poll answering a REQ sets ok_lv too (the code)
          Mutant   \* "none", or one rule broken (to show the model can fail):
                   \*   "noreq":  silence means sending the round again, never a REQ
                   \*   "nobind": S names a rung in its ack but stays where it was
                   \*   "deaf":   M ignores the rung an ack names (S thinks it heard)
                   \*   "unnamed": an ack with no new rung names none (as #386 was
                   \*             merged): a repeat of a lost one leaves M behind
                   \*   "prevr":  naming a rung leaves s_prev_lv on the round's rung
                   \*             (as #386 was merged): the two fallbacks part
                   \*   "norecvfb": the turn passing to M after a KD_BOTH that drew
                   \*             nothing sends on the same rung, without the fall-back
                   \*             (the code before #448)

None == 0
ASSUME None \notin Rungs /\ N \in Nat \ {0} /\ L \in Nat /\ W \in Nat /\ OkOnReq \in BOOLEAN

VARIABLES
    mTx,       \* M: the rung of its next round (tx_level)
    okLv,      \* M: ok_lv (-1: none yet)
    lastLv,    \* M: the rung its last round went on (last_lv)
    fellBack,  \* M: it went back to ok_lv once already
    lastKind,  \* M: its last keydown, "round", "both" (poll and round) or "req"
    mSent,     \* M: the message its rounds carry now
    mDone,     \* M: S has it, as M heard
    mHeard,    \* M: S answered M's last keydown
    sBind,     \* S: where its payload decoder listens
    sPrev,     \* S: the rung M's rounds last came on (s_prev_lv, -1: none)
    sHave,     \* S: messages it has
    wArmed,    \* S: the fallback to sPrev is armed
    lossM,     \* the channel: M's keydowns lost in a row
    lossS,     \* ...and S's
    wasted     \* ghost: rounds in a row on a rung S was not bound to

vars == <<mTx, okLv, lastLv, fellBack, lastKind, mSent, mDone, mHeard,
          sBind, sPrev, sHave, wArmed, lossM, lossS, wasted>>

Cap == W + 1                                  \* wasted saturates: a finite model
\* clean: a step in which the channel loses nothing (strong fairness below)
Lossy(lr, clean) == IF clean \/ lr >= L THEN {FALSE} ELSE BOOLEAN
Max(a, b) == IF a > b THEN a ELSE b
Names == Rungs \cup {None}
Named(n) == n # None
\* where S listens after an ack naming n
BindAck(n) == IF Named(n) /\ Mutant # "nobind" THEN n ELSE sBind
\* the rung the ack's level field carries: n, or where S listens now
AckLv(n) == IF Named(n) THEN n ELSE IF Mutant = "unnamed" THEN None ELSE sBind
\* S's s_prev_lv after an ack naming n, from p
PrevAck(n, p) == IF Named(n) /\ Mutant \notin {"nobind", "prevr"} THEN n ELSE p
\* what M takes from an ack naming n
Takes(n) == AckLv(n) # None /\ Mutant # "deaf"

\* The ACCEPT: S starts listening where M starts sending.
Init ==
    /\ mTx \in Rungs /\ sBind = mTx
    /\ okLv = -1 /\ lastLv = -1 /\ fellBack = FALSE /\ lastKind = "none"
    /\ mSent = 0 /\ mDone = TRUE /\ mHeard = TRUE
    /\ sPrev = -1 /\ sHave = 0 /\ wArmed = FALSE /\ lossM = 0 /\ lossS = 0 /\ wasted = 0

\* M sends message k on rung r (fb: this is the one fall-back to ok_lv), and
\* S answers in its slot if it decoded the round.  kind "both": a poll rides
\* with the round, and S, decoding the poll alone, answers with a sized poll.
Round(k, r, fb, kind, clean) ==
    /\ lastLv' = r /\ lastKind' = kind /\ mSent' = k
    /\ \E lostM \in Lossy(lossM, clean) :
        LET decoded == sBind = r /\ ~lostM
            pollOnly == kind = "both" /\ ~lostM /\ ~decoded
        IN
        /\ lossM' = IF lostM THEN lossM + 1 ELSE 0
        /\ wasted' = IF decoded THEN 0
                     ELSE IF sBind # r THEN (IF wasted < Cap THEN wasted + 1 ELSE Cap)
                     ELSE wasted
        /\ IF pollOnly THEN
              \* S missed the round, heard the poll: a sized poll for it on rung l
              /\ UNCHANGED <<sHave, sPrev>>
              /\ \E l \in Rungs, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = l
                   /\ wArmed' = (sPrev # -1 /\ l # sPrev)
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ mDone' = FALSE
                   /\ IF lostS THEN
                         /\ mHeard' = FALSE
                         /\ mTx' = r /\ fellBack' = fb /\ okLv' = okLv
                      ELSE
                         /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ okLv' = IF OkOnReq THEN r ELSE okLv
                         /\ mTx' = l
           ELSE IF ~decoded THEN
              \* S decoded nothing of M: the fallback, if armed, fires
              /\ sBind' = IF wArmed THEN sPrev ELSE sBind
              /\ wArmed' = FALSE
              /\ mDone' = FALSE /\ mHeard' = FALSE
              /\ lossS' = lossS
              /\ mTx' = r /\ fellBack' = fb
              /\ UNCHANGED <<okLv, sPrev, sHave>>
           ELSE
              \* S has it: it answers with an ack, naming a rung or not
              /\ sHave' = Max(sHave, k) /\ wArmed' = FALSE
              /\ \E n \in Names, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = BindAck(n) /\ sPrev' = PrevAck(n, r)
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ IF lostS THEN
                         /\ mDone' = FALSE /\ mHeard' = FALSE
                         /\ mTx' = r /\ fellBack' = fb /\ okLv' = okLv
                      ELSE
                         /\ mDone' = TRUE /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ okLv' = IF Takes(n) THEN AckLv(n) ELSE r
                         /\ mTx' = IF Takes(n) THEN AckLv(n) ELSE r

\* M asks S where it stands about message mSent: a control frame, which S
\* decodes on any rung.
Req(clean) ==
    /\ lastKind' = "req" /\ wasted' = wasted
    /\ UNCHANGED <<lastLv, mSent>>
    /\ \E lostM \in Lossy(lossM, clean) :
        IF lostM THEN
           /\ sBind' = IF wArmed THEN sPrev ELSE sBind
           /\ wArmed' = FALSE
           /\ mHeard' = FALSE /\ lossM' = lossM + 1
           /\ UNCHANGED <<mTx, okLv, fellBack, mDone, sPrev, sHave, lossS>>
        ELSE
           /\ UNCHANGED sHave /\ lossM' = 0
           /\ IF sHave >= mSent THEN
                 \* it has it: an ack, as after the round
                 \E n \in Names, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = BindAck(n) /\ sPrev' = PrevAck(n, sPrev) /\ wArmed' = FALSE
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ IF lostS THEN
                         /\ mHeard' = FALSE
                         /\ UNCHANGED <<mTx, okLv, fellBack, mDone>>
                      ELSE
                         /\ mDone' = TRUE /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ okLv' = IF Takes(n) THEN AckLv(n)
                                    ELSE IF OkOnReq THEN lastLv ELSE okLv
                         /\ mTx' = IF Takes(n) THEN AckLv(n) ELSE mTx
              ELSE
                 \* it missed it: a sized poll for it on rung l
                 \E l \in Rungs, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = l /\ sPrev' = sPrev
                   /\ wArmed' = (sPrev # -1 /\ l # sPrev)
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ mDone' = FALSE
                   /\ IF lostS THEN
                         /\ mHeard' = FALSE
                         /\ UNCHANGED <<mTx, okLv, fellBack>>
                      ELSE
                         /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ okLv' = IF OkOnReq THEN lastLv ELSE okLv
                         /\ mTx' = l

\* The keydowns M may make for a round on rung r: a poll rides with it where r
\* is not yet the rung M has an answer on (a probe).
Kinds(r) == IF r # okLv THEN {"round", "both"} ELSE {"round"}

\* M can go back to ok_lv: its last round was on another rung, unanswered.
CanFallBack == okLv # -1 /\ lastLv # okLv /\ ~fellBack

\* The turn passes to M after a keydown of poll and round that drew nothing:
\* m_decide_recv, not m_decide_send.  With #448 it too goes back to ok_lv once.
TurnMine(clean) ==
    IF Mutant # "norecvfb" /\ CanFallBack
    THEN Round(mSent, okLv, TRUE, "round", clean)
    ELSE Round(mSent, mTx, fellBack, "round", clean)

\* M's decision, as m_decide_send and m_decide_recv make it.
MStep(clean) ==
    \/ /\ mDone /\ mSent < N                    \* the next message
       /\ \E kd \in Kinds(mTx) : Round(mSent + 1, mTx, fellBack, kd, clean)
    \/ /\ ~mDone /\ mHeard                      \* S polled for it: send it there
       /\ \E kd \in Kinds(mTx) : Round(mSent, mTx, fellBack, kd, clean)
    \/ /\ ~mDone /\ ~mHeard                     \* silence
       /\ IF Mutant = "noreq" THEN Round(mSent, mTx, fellBack, "round", clean)
          ELSE IF lastKind = "both" THEN (Req(clean) \/ TurnMine(clean))
          ELSE IF lastKind = "round" /\ CanFallBack
          THEN Round(mSent, okLv, TRUE, "round", clean)
          ELSE Req(clean)
MNext == MStep(FALSE)

Finished == mDone /\ mSent = N
Done == Finished /\ UNCHANGED vars

Next == MNext \/ Done

\* M keeps going; and the channel is not an adversary for ever: an exchange
\* that could go through loss-free, again and again, does at last.
\* (Per kind of keydown that S can decode: one getting through must not
\* stand in for another -- a channel losing every round S listens for, and
\* none it does not, is no fairer than one losing everything.)
Spec == Init /\ [][Next]_vars /\ WF_vars(MNext)
        /\ SF_vars(MStep(TRUE) /\ lastKind' = "round" /\ lastLv' = sBind)
        /\ SF_vars(MStep(TRUE) /\ lastKind' = "req")

\* ---- properties ----
TypeOK ==
    /\ mTx \in Rungs /\ sBind \in Rungs /\ lastKind \in {"none", "round", "both", "req"}
    /\ okLv \in Rungs \cup {-1} /\ lastLv \in Rungs \cup {-1} /\ sPrev \in Rungs \cup {-1}
    /\ mSent \in 0 .. N /\ sHave \in 0 .. N /\ lossM \in 0 .. L /\ lossS \in 0 .. L
    /\ wasted \in 0 .. Cap
Delivered == <>Finished
Agree == mHeard => mTx = sBind
Wasted == wasted <= W
\* the finite model: wasted saturates at Cap (TLC needs a bound for liveness)
Bounded == wasted <= Cap
=============================================================================
