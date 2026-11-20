---------------------------- MODULE FloorFallback -----------------------------
(***************************************************************************)
(* RungAgreement and FloorSilence combined: the caller's fall-back onto   *)
(* the MFSK floor when the callee confirmed the rung above and stayed on  *)
(* it (#529).  Mirrors datalink_arq/carousel.c:                           *)
(*                                                                         *)
(*   RungAgreement's rules (rounds on tx_level, ok_lv, the one fall-back  *)
(*   to ok_lv in m_decide_send and m_decide_recv, the REQ, S binding      *)
(*   where it says it listens and its CAR_T_W fallback to s_prev_lv),     *)
(*   with the floor as one of the rungs (Floor).  At the floor S answers  *)
(*   a round with a pattern (an ack naming nothing: s_idle_rung returns 0 *)
(*   there) or, every FLOOR_POLL_EVERY rounds, a poll naming the next     *)
(*   rung, which leaves M's ok_lv at the floor (m_on_ctl).                *)
(*                                                                         *)
(*   At the floor, silence means "keep going" (m_decide_send): a round on *)
(*   the floor that drew nothing is followed by another, while the count  *)
(*   of silent floor rounds (floor_silent, ++ on each, reset only when M  *)
(*   hears S) stays within the cap: FB after an unconfirmed fall-back     *)
(*   (fell_back, #529), FMax (FLOOR_SILENT_MAX) otherwise.  Past it, the  *)
(*   fall-back, then the REQ.                                             *)
(*                                                                         *)
(*   At FL silent floor rounds (FLOOR_SILENT_LOSS, ctl_deaf_on_floor_     *)
(*   silence) M puts its control on MFSK (CAR_DEAF_LOSS): S decodes a     *)
(*   REQ there only bound to the floor.  Hearing S clears it; so may the  *)
(*   hold's expiry (LOSS_HOLD_MS), at any unanswered REQ.                 *)
(*                                                                         *)
(*   S falls back to the floor after FS exchanges with nothing of M's     *)
(*   decoded (S_FALLBACK_MS), once a silence, and stays there through     *)
(*   its round timer (FloorSilence).                                      *)
(*                                                                         *)
(* The case: M on the floor, S's floor poll moves it to rung r, ok_lv     *)
(* stays at the floor.  S decodes M's round on r and confirms it          *)
(* (s_prev_lv = r) in an answer that is lost.  M falls back to ok_lv, the *)
(* floor, where S is not listening; S, which has heard M, arms nothing.   *)
(* Only the REQ (on DATAC16, or on MFSK once S is at the floor) or S's    *)
(* own silence timer brings them together.  The count is cumulative, as  *)
(* in the code: the rule reads "after the fall-back, ask once the floor   *)
(* has been silent FB times since M last heard S".                        *)
(*                                                                         *)
(* Not modelled: the channel going down (FloorSilence: the floor delivers *)
(* either way), time (exchanges are counted; specs/turns has the timing), *)
(* KD_BOTH on the floor (the floor streams rounds, m_kind KD_ROUND), and  *)
(* patterns (a floor round is decoded or not, as any other).              *)
(*                                                                         *)
(* Properties: Delivered, Agree, WastedF (rounds sent where S does not    *)
(* listen, since the last loss, at most W), as in RungAgreement; and      *)
(* WastedFB, the same rounds counted only while M's fall-back is          *)
(* unconfirmed, at most B: the bound the cap FB sets.                     *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS Rungs,   \* the rungs above the floor ({1} or {1, 2} is enough)
          N,       \* messages M sends
          L,       \* most keydowns the channel loses in a row
          W,       \* the bound WastedF checks
          B,       \* the bound WastedFB checks
          FB,      \* silent floor rounds allowed after an unconfirmed fall-back (#529: 1)
          FMax,    \* ...otherwise (FLOOR_SILENT_MAX: 6)
          FL,      \* silent floor rounds that put M's control on MFSK (FLOOR_SILENT_LOSS: 2)
          FS       \* S's silence, in exchanges, before it listens at the floor

Floor == 0
None  == -2
ASSUME Floor \notin Rungs /\ None \notin Rungs /\ N \in Nat \ {0} /\ L \in Nat /\ W \in Nat /\ B \in Nat
ASSUME FB \in Nat /\ FMax \in Nat /\ FL \in Nat \ {0} /\ FS \in Nat \ {0}

All == Rungs \cup {Floor}

VARIABLES
    mTx,         \* M: the rung of its next round (tx_level)
    okLv,        \* M: ok_lv (-1: none yet)
    lastLv,      \* M: the rung its last round went on (last_lv)
    fellBack,    \* M: it went back to ok_lv once already
    lastKind,    \* M: its last keydown, "round", "both" or "req"
    mSent,       \* M: the message its rounds carry now
    mDone,       \* M: S has it, as M heard
    mHeard,      \* M: S answered M's last keydown
    floorSilent, \* M: silent floor rounds since it last heard S (floor_silent)
    mCtlFloor,   \* M: its control on MFSK after silent floor rounds (CAR_DEAF_LOSS)
    sBind,       \* S: where its payload decoder listens
    sPrev,       \* S: the rung M's rounds last came on (s_prev_lv, -1: none)
    sHave,       \* S: messages it has
    wArmed,      \* S: the fallback to sPrev is armed (CAR_T_W)
    sSilent,     \* S: exchanges since it last decoded anything of M's
    sFell,       \* S: it fell back to the floor in this silence
    lossM, lossS,
    wf,          \* ghost: rounds on a rung S was not bound to, since the last loss
    wfb          \* ghost: the same, counted only while M's fall-back is unconfirmed

vars == <<mTx, okLv, lastLv, fellBack, lastKind, mSent, mDone, mHeard, floorSilent, mCtlFloor,
          sBind, sPrev, sHave, wArmed, sSilent, sFell, lossM, lossS, wf, wfb>>

Max(a, b) == IF a > b THEN a ELSE b
Min(a, b) == IF a < b THEN a ELSE b
WCap  == Max(W, B) + 1             \* wf, wfb saturate: a finite model
FsCap == FMax + 1                  \* so does floor_silent
Lossy(lr, clean) == IF clean \/ lr >= L THEN {FALSE} ELSE BOOLEAN
Names == All \cup {None}
\* What an ack may name: nothing while S polls at the floor (s_idle_rung returns
\* 0 there: io.pattern and poll_level 0), any rung otherwise.
AckNames(r) == IF r = Floor THEN {None} ELSE Names
BindAck(n) == IF n # None THEN n ELSE sBind
AckLv(n)   == IF n # None THEN n ELSE sBind
PrevAck(n, p) == IF n # None THEN n ELSE p

Init ==
    /\ mTx \in All /\ sBind = mTx
    /\ okLv = -1 /\ lastLv = -1 /\ fellBack = FALSE /\ lastKind = "none"
    /\ mSent = 0 /\ mDone = TRUE /\ mHeard = TRUE /\ floorSilent = 0 /\ mCtlFloor = FALSE
    /\ sPrev = -1 /\ sHave = 0 /\ wArmed = FALSE /\ sSilent = 0 /\ sFell = FALSE
    /\ lossM = 0 /\ lossS = 0 /\ wf = 0 /\ wfb = 0

\* S decoded nothing of M's keydown: its silence grows; S_FALLBACK_MS takes it
\* to the floor once a silence, else CAR_T_W back to sPrev (not off a floor it
\* fell back to).
SDeaf ==
    /\ sSilent' = Min(sSilent + 1, FS)
    /\ IF sSilent + 1 >= FS /\ ~sFell THEN
          sBind' = Floor /\ sFell' = TRUE /\ wArmed' = FALSE
       ELSE IF wArmed /\ ~(sFell /\ sBind = Floor) THEN
          sBind' = sPrev /\ wArmed' = FALSE /\ sFell' = sFell
       ELSE UNCHANGED <<sBind, wArmed, sFell>>
    /\ UNCHANGED <<sPrev, sHave>>

\* S decoded a keydown of M's.
SHeard == sSilent' = 0 /\ sFell' = FALSE

\* M heard S: its silence ends.
MHeard == floorSilent' = 0 /\ mCtlFloor' = FALSE

\* M sends message k on rung r (fb: this is the one fall-back to ok_lv); S
\* answers if it decoded the round.  fs, cf: M's floor_silent and control
\* placement if S's answer is not heard.
Round(k, r, fb, kind, fs, cf, clean) ==
    /\ lastLv' = r /\ lastKind' = kind /\ mSent' = k
    /\ \E lostM \in Lossy(lossM, clean),
         lostR \in (IF kind = "both" THEN Lossy(lossM, clean) ELSE {FALSE}) :
        LET decoded  == sBind = r /\ ~lostM /\ ~lostR
            pollOnly == kind = "both" /\ ~lostM /\ ~decoded
            Bw == IF lostM \/ lostR \/ decoded THEN 0
                  ELSE IF sBind # r THEN Min(wf + 1, WCap) ELSE wf
        IN
        /\ lossM' = IF lostM \/ lostR THEN lossM + 1 ELSE 0
        /\ IF pollOnly THEN
              /\ SHeard /\ UNCHANGED <<sHave, sPrev>>
              /\ \E l \in All, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = l
                   /\ wArmed' = (sPrev # -1 /\ l # sPrev)
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ mDone' = FALSE
                   /\ IF lostS THEN
                         /\ wf' = 0 /\ mHeard' = FALSE /\ floorSilent' = fs /\ mCtlFloor' = cf
                         /\ mTx' = r /\ fellBack' = fb /\ okLv' = okLv
                      ELSE
                         /\ wf' = Bw /\ mHeard' = TRUE /\ fellBack' = FALSE /\ MHeard
                         /\ okLv' = r /\ mTx' = l
           ELSE IF ~decoded THEN
              /\ SDeaf
              /\ mDone' = FALSE /\ mHeard' = FALSE /\ wf' = Bw
              /\ floorSilent' = fs /\ mCtlFloor' = cf
              /\ lossS' = lossS
              /\ mTx' = r /\ fellBack' = fb
              /\ UNCHANGED okLv
           ELSE
              /\ SHeard
              /\ sHave' = Max(sHave, k)
              /\ \/ \* an ack (a pattern at the floor), naming a rung or not
                    /\ wArmed' = FALSE
                    /\ \E n \in AckNames(r), lostS \in Lossy(lossS, clean) :
                         /\ sBind' = BindAck(n) /\ sPrev' = PrevAck(n, r)
                         /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                         /\ IF lostS THEN
                               /\ wf' = 0 /\ mDone' = FALSE /\ mHeard' = FALSE
                               /\ floorSilent' = fs /\ mCtlFloor' = cf
                               /\ mTx' = r /\ fellBack' = fb /\ okLv' = okLv
                            ELSE
                               /\ wf' = Bw /\ mDone' = TRUE /\ mHeard' = TRUE /\ fellBack' = FALSE
                               /\ MHeard
                               /\ okLv' = AckLv(n) /\ mTx' = AckLv(n)
                 \/ \* at the floor, S's poll (FLOOR_POLL_EVERY) naming the next rung l:
                    \* M's ok_lv is the floor, the rung of the round it answers
                    \* (m_on_ctl: pat_waiting or floor_waiting), and S, asking for
                    \* a rung M's rounds have not come on, arms CAR_T_W
                    /\ r = Floor
                    /\ \E l \in All, lostS \in Lossy(lossS, clean) :
                         /\ sBind' = l /\ sPrev' = r /\ wArmed' = (l # r)
                         /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                         /\ IF lostS THEN
                               /\ wf' = 0 /\ mDone' = FALSE /\ mHeard' = FALSE
                               /\ floorSilent' = fs /\ mCtlFloor' = cf
                               /\ mTx' = r /\ fellBack' = fb /\ okLv' = okLv
                            ELSE
                               /\ wf' = Bw /\ mDone' = TRUE /\ mHeard' = TRUE /\ fellBack' = FALSE
                               /\ MHeard
                               /\ okLv' = r /\ mTx' = l
        \* after the branches: fellBack' is set there
        /\ wfb' = IF ~fellBack' \/ lostM \/ lostR \/ decoded THEN 0
                  ELSE IF sBind # r THEN Min(wfb + 1, WCap) ELSE wfb

\* M asks S where it stands: on DATAC16, which S decodes on any rung, or on
\* MFSK (mCtlFloor), which S decodes only bound to the floor.
Req(clean) ==
    /\ lastKind' = "req"
    /\ UNCHANGED <<lastLv, mSent>>
    /\ \E lostM \in Lossy(lossM, clean) :
        IF lostM \/ (mCtlFloor /\ sBind # Floor) THEN
           /\ SDeaf
           /\ mHeard' = FALSE /\ floorSilent' = floorSilent
           /\ mCtlFloor' \in {mCtlFloor, FALSE}          \* LOSS_HOLD_MS may expire
           /\ lossM' = IF lostM THEN lossM + 1 ELSE lossM
           /\ wf' = IF lostM THEN 0 ELSE wf
           /\ UNCHANGED <<mTx, okLv, fellBack, mDone, lossS>>
        ELSE
           /\ SHeard /\ UNCHANGED sHave /\ lossM' = 0
           /\ IF sHave >= mSent THEN
                 \E n \in AckNames(sBind), lostS \in Lossy(lossS, clean) :
                   /\ sBind' = BindAck(n) /\ sPrev' = PrevAck(n, sPrev) /\ wArmed' = FALSE
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ IF lostS THEN
                         /\ wf' = 0 /\ mHeard' = FALSE
                         /\ UNCHANGED <<mTx, okLv, fellBack, mDone, floorSilent, mCtlFloor>>
                      ELSE
                         /\ wf' = wf /\ mDone' = TRUE /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ MHeard
                         /\ okLv' = AckLv(n) /\ mTx' = AckLv(n)
              ELSE
                 \E l \in All, lostS \in Lossy(lossS, clean) :
                   /\ sBind' = l /\ sPrev' = sPrev
                   /\ wArmed' = (sPrev # -1 /\ l # sPrev)
                   /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
                   /\ mDone' = FALSE
                   /\ IF lostS THEN
                         /\ wf' = 0 /\ mHeard' = FALSE
                         /\ UNCHANGED <<mTx, okLv, fellBack, floorSilent, mCtlFloor>>
                      ELSE
                         /\ wf' = wf /\ mHeard' = TRUE /\ fellBack' = FALSE
                         /\ MHeard
                         /\ okLv' = lastLv /\ mTx' = l
    /\ wfb' = IF fellBack' THEN wfb ELSE 0           \* a REQ sends no round

\* KD_BOTH only above the floor.
Kinds(r) == IF r = Floor THEN {"round"} ELSE {"round", "both"}

CanFallBack == okLv # -1 /\ lastLv # okLv /\ ~fellBack

\* The floor rule of m_decide_send: another floor round while the cap allows.
OnFloor  == lastKind = "round" /\ lastLv = Floor
FloorCap == IF fellBack THEN FB ELSE FMax
FsNext   == Min(floorSilent + 1, FsCap)
Continue(clean) ==
    Round(mSent, Floor, fellBack, "round", FsNext, mCtlFloor \/ FsNext >= FL, clean)

\* m_decide_send after silence: the floor rule, then the fall-back, then the REQ.
SilentSend(clean) ==
    IF OnFloor /\ floorSilent + 1 <= FloorCap THEN Continue(clean)
    ELSE IF CanFallBack THEN
       Round(mSent, okLv, TRUE, "round", IF OnFloor THEN FsNext ELSE floorSilent, mCtlFloor, clean)
    ELSE Req(clean)

\* m_decide_recv after a KD_BOTH that drew nothing: the fall-back once (#448).
TurnMine(clean) ==
    IF CanFallBack THEN Round(mSent, okLv, TRUE, "round", floorSilent, mCtlFloor, clean)
    ELSE Round(mSent, mTx, fellBack, "round", floorSilent, mCtlFloor, clean)

MStep(clean) ==
    \/ /\ mDone /\ mSent < N
       /\ \E kd \in Kinds(mTx) : Round(mSent + 1, mTx, fellBack, kd, floorSilent, mCtlFloor, clean)
    \/ /\ ~mDone /\ mHeard
       /\ \E kd \in Kinds(mTx) : Round(mSent, mTx, fellBack, kd, floorSilent, mCtlFloor, clean)
    \/ /\ ~mDone /\ ~mHeard
       /\ IF lastKind = "both" THEN (Req(clean) \/ TurnMine(clean))
          ELSE SilentSend(clean)
MNext == MStep(FALSE)

Finished == mDone /\ mSent = N
Done == Finished /\ UNCHANGED vars
Next == MNext \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(MNext)
        /\ SF_vars(MStep(TRUE) /\ lastKind' = "round" /\ lastLv' = sBind)
        /\ SF_vars(MStep(TRUE) /\ lastKind' = "req")

TypeOK ==
    /\ mTx \in All /\ sBind \in All /\ lastKind \in {"none", "round", "both", "req"}
    /\ okLv \in All \cup {-1} /\ lastLv \in All \cup {-1} /\ sPrev \in All \cup {-1}
    /\ mSent \in 0 .. N /\ sHave \in 0 .. N /\ lossM \in 0 .. L /\ lossS \in 0 .. L
    /\ floorSilent \in 0 .. FsCap /\ sSilent \in 0 .. FS /\ wf \in 0 .. WCap /\ wfb \in 0 .. WCap
    /\ fellBack \in BOOLEAN /\ mCtlFloor \in BOOLEAN /\ wArmed \in BOOLEAN /\ sFell \in BOOLEAN
Delivered == <>Finished
Agree == mHeard => mTx = sBind
WastedF == wf <= W
\* After an unconfirmed fall-back, rounds sent where S does not listen (#529).
WastedFB == wfb <= B
=============================================================================
