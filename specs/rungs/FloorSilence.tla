---------------------------- MODULE FloorSilence ------------------------------
(***************************************************************************)
(* Silence, and the floor: what keeps a session alive when the channel    *)
(* falls below every mode but the MFSK floor.  Mirrors carousel.c:         *)
(*                                                                         *)
(*   The channel is up, then at any step may go down for good: only the  *)
(*   floor decodes then, either way (frames, MFSK control, patterns).     *)
(*   Up, everything does.  Any keydown can also be lost, at most L in a   *)
(*   row.  (Down for good is the case at issue; a channel that comes back *)
(*   is measured again, and the floor delivers either way.)               *)
(*                                                                         *)
(*   M, the caller, keys a round on mRung, or after a silent exchange its *)
(*   control (a handover, a REQ) on mCtl: DATAC16 ("c16"), which S's      *)
(*   control decoder takes on any rung while the channel is up, or MFSK   *)
(*   ("floor"), which S decodes only when bound to the floor.  Silence:   *)
(*   after FM exchanges with nothing of S's heard, M takes both ways to   *)
(*   be below the control mode (car_on_tx_done, SILENT_MS): its control  *)
(*   and its rounds go on the floor, and its frames tell S so, so S's    *)
(*   answers go there too.                                                *)
(*                                                                         *)
(*   S, the callee, answers what it decodes, naming the rung M's next     *)
(*   round goes on -- any while the link is up (the SNR's, the next one), *)
(*   the floor once it is down, as the reading of what it decoded says    *)
(*   (choose_level's fall_level) -- and binds there;                      *)
(*   a rung other than the one M's rounds came on arms its round timer    *)
(*   (CAR_T_W): with nothing decoded, back to that rung (sPrev).  After   *)
(*   FS exchanges with nothing decoded S falls back to the floor          *)
(*   (S_FALLBACK_MS), once a silence: the timer is armed when S hears M.  *)
(*   A fallen-back S stays at the floor through its round timer.          *)
(*                                                                         *)
(* Not modelled: rounds lost in part, the turn timing (specs/turns), what *)
(* M and S do when the channel comes back (they measure it again: the    *)
(* floor delivers either way).                                            *)
(*                                                                         *)
(* Property: Delivered, every message gets through, with the channel     *)
(* down for ever if the adversary likes.                                  *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS N,      \* messages M sends
          L,      \* most keydowns the channel loses in a row (each way)
          FM,     \* M's silence, in exchanges, before its control goes to the floor
          FS,     \* S's silence, in exchanges, before it listens at the floor
          Mutant  \* "none", or one rule broken (to show the model can fail):
                  \*   "nosilence": M never takes the control mode to be out
                  \*   "nostay":    S's round timer takes it off the floor it fell back to
                  \*   "noread":    S, having read the channel down, names a faster rung

Floor == 0
Fast  == 1
Rungs == {Floor, Fast}

VARIABLES
    up,        \* the channel: TRUE, every mode decodes; FALSE, only the floor
    mRung,     \* M: the rung of its next round
    mCtl,      \* M: where its control goes, "c16" or "floor"
    mSilent,   \* M: exchanges since it last heard S
    mSent,     \* M: the message its rounds carry
    mDone,     \* M: S has it, as M heard
    sBind,     \* S: where its payload decoder listens
    sPrev,     \* S: the rung M's rounds last came on
    sArmed,    \* S: its round timer is armed (it asked for a rung they never came on)
    sSilent,   \* S: exchanges since it last decoded anything of M's
    sFell,     \* S: it fell back to the floor in this silence
    sHave,     \* S: messages it has
    lossM, lossS

vars == <<up, mRung, mCtl, mSilent, mSent, mDone, sBind, sPrev, sArmed, sSilent, sFell, sHave,
          lossM, lossS>>

Lossy(lr, clean) == IF clean \/ lr >= L THEN {FALSE} ELSE BOOLEAN
Max(a, b) == IF a > b THEN a ELSE b
Decodes(r) == up \/ r = Floor                        \* a keydown on rung r gets through
SCtl == IF mCtl = "floor" THEN Floor ELSE Fast       \* S answers at the floor once M says so

Init ==
    /\ up = TRUE /\ mRung = Fast /\ sBind = Fast /\ sPrev = Fast
    /\ mCtl = "c16" /\ mSilent = 0 /\ mSent = 1 /\ mDone = FALSE
    /\ sArmed = FALSE /\ sSilent = 0 /\ sFell = FALSE /\ sHave = 0 /\ lossM = 0 /\ lossS = 0

\* S decoded nothing of M's keydown: its silence grows, and its timers fire.
\* The round timer runs to the end of the round S asked for, which may come
\* before S's fallback or after it: it fires on any silent exchange (w).
SDeaf ==
    /\ sSilent' = IF sSilent < FS THEN sSilent + 1 ELSE FS
    /\ \E w \in BOOLEAN :
        IF sSilent + 1 >= FS /\ ~sFell THEN                \* S_FALLBACK_MS; the round
           /\ sBind' = Floor /\ sFell' = TRUE /\ UNCHANGED <<sPrev, sArmed>>  \* timer stays armed
        ELSE IF w /\ sArmed /\ ~(sFell /\ sBind = Floor /\ Mutant # "nostay") THEN  \* CAR_T_W
           /\ sBind' = sPrev /\ sArmed' = FALSE /\ UNCHANGED <<sFell, sPrev>>
        ELSE UNCHANGED <<sBind, sArmed, sFell, sPrev>>
    /\ UNCHANGED sHave

\* M heard nothing back: its silence grows; at FM, the floor.
MDeaf ==
    /\ mSilent' = IF mSilent < FM THEN mSilent + 1 ELSE FM
    /\ IF mSilent + 1 >= FM /\ Mutant # "nosilence"
       THEN mCtl' = "floor" /\ mRung' = Floor
       ELSE UNCHANGED <<mCtl, mRung>>
    /\ UNCHANGED mDone

\* One exchange: M's keydown (a round, or control after silence), S's answer.
\* Only while M has a message S has not acknowledged.
Exchange(clean) ==
    /\ ~mDone
    /\ up' \in (IF up THEN BOOLEAN ELSE {FALSE})
    /\ \E lostM \in Lossy(lossM, clean), lostS \in Lossy(lossS, clean),
          n \in (IF up \/ Mutant = "noread" THEN Rungs ELSE {Floor}) :
        LET round   == mSilent = 0                   \* after silence M sends control
            onRung  == IF round THEN mRung ELSE SCtl  \* the rung S must listen on for it
            heardS  == ~lostM /\ Decodes(onRung) /\
                       (IF round \/ mCtl = "floor" THEN sBind = onRung ELSE TRUE)
            heardM  == ~lostS /\ Decodes(SCtl)       \* S's answer, on its control mode
        IN
        /\ lossM' = IF lostM THEN lossM + 1 ELSE 0
        /\ IF ~heardS THEN
              /\ SDeaf /\ MDeaf /\ lossS' = lossS
              /\ UNCHANGED <<mSent>>
           ELSE
              \* S has M's keydown; it answers naming rung n for the next round
              /\ sSilent' = 0 /\ sFell' = FALSE
              /\ sHave' = IF round THEN Max(sHave, mSent) ELSE sHave
              /\ sBind' = n /\ sPrev' = IF round THEN mRung ELSE sPrev
              /\ sArmed' = (n # (IF round THEN mRung ELSE sPrev))
              /\ lossS' = IF lostS THEN lossS + 1 ELSE 0
              /\ IF heardM THEN
                    /\ mSilent' = 0 /\ mRung' = n
                    /\ mDone' = (sHave' >= mSent)
                    /\ mCtl' = mCtl
                    /\ mSent' = mSent
                 ELSE
                    /\ MDeaf /\ UNCHANGED <<mSent>>

\* M moves to the next message once S has this one.
NextMsg ==
    /\ mDone /\ mSent < N
    /\ mSent' = mSent + 1 /\ mDone' = FALSE
    /\ UNCHANGED <<up, mRung, mCtl, mSilent, sBind, sPrev, sArmed, sSilent, sFell, sHave, lossM, lossS>>

Finished == mDone /\ mSent = N
Step(clean) == ~Finished /\ (NextMsg \/ Exchange(clean))
Done == Finished /\ UNCHANGED vars
Next == Step(FALSE) \/ Done

\* M keeps going; an exchange that could go through loss-free, again and
\* again, does at last -- per kind, as in RungAgreement: a round S can
\* decode, and M's control.  The channel itself may stay down for ever.
CleanRound == Step(TRUE) /\ mSilent = 0 /\ mRung = sBind /\ Decodes(mRung)
Spec == Init /\ [][Next]_vars /\ WF_vars(Step(FALSE))
        /\ SF_vars(CleanRound) /\ SF_vars(Step(TRUE))

TypeOK ==
    /\ up \in BOOLEAN /\ mRung \in Rungs /\ sBind \in Rungs /\ sPrev \in Rungs
    /\ mCtl \in {"c16", "floor"} /\ mSilent \in 0 .. FM /\ sSilent \in 0 .. FS
    /\ mSent \in 1 .. N /\ sHave \in 0 .. N /\ lossM \in 0 .. L /\ lossS \in 0 .. L
Delivered == <>Finished
=============================================================================
