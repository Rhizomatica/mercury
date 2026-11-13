------------------------------ MODULE CtlDeaf --------------------------------
(***************************************************************************)
(* When M, the caller, takes itself deaf to the control mode, and when it *)
(* stops: car_t.ctl_deaf in carousel.c (car_on_frame, ctl_deaf_on_asks,   *)
(* ctl_loss_expired).  Deaf, M says so in every frame it sends, and S's   *)
(* control goes on the MFSK floor.                                        *)
(*                                                                         *)
(*   band: where M's smoothed reading of S (snr_ema) sits, against        *)
(*   CTL_DEAF_ON_DB (-7.5), CTL_DEAF_OFF_DB (-6.0) and CTL_LOSS_OFF_DB    *)
(*   (-3.0): "low" < ON <= "hyst" < OFF <= "mid" < LOSS_OFF <= "high".    *)
(*   The channel is free: any frame may read any band.                    *)
(*                                                                         *)
(*   deaf:                                                                 *)
(*     "snr"    the reading: set below ON, kept below OFF.                *)
(*     "hard"   the connect was heard only on MFSK: an MFSK frame's       *)
(*              reading does not clear it, an OFDM frame's does.           *)
(*     "mfsktx" S transmits only MFSK: for good.                          *)
(*     "loss"   two asks unanswered while S is heard and the reading is   *)
(*              below LOSS_OFF.  Ends on a reading in "high", or after    *)
(*              LOSS_HOLD_MS (Hold ticks of CAR_T_F), back to what the    *)
(*              reading says.                                              *)
(*                                                                         *)
(*   reqs: asks unanswered in a row (capped at 2), fellBack: a round     *)
(*   unanswered, heard: S decoded within SILENT_MS.  Any frame of S's    *)
(*   counts as heard; an answer also clears reqs and fellBack.           *)
(*                                                                         *)
(* Not modelled: what S does with the report (it only picks the mode of  *)
(* its control), the reading's arithmetic (the bands stand for it), the  *)
(* rungs (specs/rungs), and the silence rule's own floor (FloorSilence). *)
(*                                                                         *)
(* Properties:                                                             *)
(*   LossEnds       LOSS, once set, ends -- even if the reading stays in *)
(*                  "hyst" or "mid" for ever and asks keep going          *)
(*                  unanswered (it may be set again: each spell ends).    *)
(*   LossNeedsAsks  LOSS is set only on two asks unanswered, S heard,    *)
(*                  the reading below LOSS_OFF.                            *)
(*   HardNeedsOfdm  HARD ends only on an OFDM frame (or for MFSK_TX,     *)
(*                  never: it cannot be left, by design).                 *)
(*   MfskTxStays    MFSK_TX is for good.                                  *)
(*                                                                         *)
(* Mutant: "none", the code; "nohold", LOSS held to the end of the       *)
(* session (#538 before review: ends only on "high"); "fellback", an     *)
(* unanswered round counted as a missed answer (also #538 before review). *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Hold,     \* LOSS_HOLD_MS, in ticks of the model's clock (>= 1)
          Mutant    \* "none", "nohold", "fellback"

Bands  == {"low", "hyst", "mid", "high"}
Deafs  == {"none", "snr", "hard", "mfsktx", "loss"}

VARIABLES deaf, band, reqs, fellBack, heard, hold, lastMfsk
vars == <<deaf, band, reqs, fellBack, heard, hold, lastMfsk>>

TypeOK == /\ deaf \in Deafs
          /\ band \in Bands
          /\ reqs \in 0..2
          /\ fellBack \in BOOLEAN
          /\ heard \in BOOLEAN
          /\ hold \in 0..Hold
          /\ lastMfsk \in BOOLEAN

\* The SNR rule (car_on_frame): not deaf, deaf below ON; deaf, still below OFF.
SnrRule(was, b) == IF (IF was THEN b \in {"low", "hyst"} ELSE b = "low")
                   THEN "snr" ELSE "none"

\* What a frame read in band b, on MFSK or not, does to deaf.
OnReading(d, b, mfsk) ==
    CASE d = "loss"                -> IF b = "high" THEN "none" ELSE "loss"
      [] d = "mfsktx"              -> "mfsktx"
      [] d = "hard" /\ mfsk        -> "hard"
      [] OTHER                     -> SnrRule(d # "none", b)

Init == /\ deaf \in {"none", "snr", "hard", "mfsktx"}   \* seeded by the connect
        /\ band \in Bands
        /\ reqs = 0
        /\ fellBack = FALSE
        /\ heard = TRUE
        /\ hold = 0
        /\ lastMfsk \in BOOLEAN

\* A frame of S's: an answer (clears the misses) or anything else.
Frame(b, mfsk, answer) ==
    /\ band' = b
    /\ lastMfsk' = mfsk
    /\ heard' = TRUE
    /\ deaf' = OnReading(deaf, b, mfsk)
    /\ hold' = IF OnReading(deaf, b, mfsk) = "loss" THEN hold ELSE 0
    /\ reqs' = IF answer THEN 0 ELSE reqs
    /\ fellBack' = IF answer THEN FALSE ELSE fellBack

\* An ask of mine gone unanswered (m_decide_send): ctl_deaf_on_asks.
Missed(r) == IF Mutant = "fellback" THEN r + (IF fellBack THEN 1 ELSE 0) ELSE r

AskUnanswered ==
    LET r == IF reqs < 2 THEN reqs + 1 ELSE 2
        go == Missed(r) >= 2 /\ deaf = "none" /\ heard /\ band # "high"
    IN /\ reqs' = r
       /\ deaf' = IF go THEN "loss" ELSE deaf
       /\ hold' = IF go THEN Hold ELSE hold
       /\ UNCHANGED <<band, fellBack, heard, lastMfsk>>

\* A round of mine gone unanswered: the next goes back a rung (fell_back).
RoundUnanswered ==
    /\ fellBack' = TRUE
    /\ UNCHANGED <<deaf, band, reqs, heard, hold, lastMfsk>>

\* SILENT_MS passes with nothing of S's decoded.
Silence ==
    /\ heard' = FALSE
    /\ UNCHANGED <<deaf, band, reqs, fellBack, hold, lastMfsk>>

\* Time passes: CAR_T_F counts down LOSS's hold (ctl_loss_expired).
Tick ==
    /\ IF deaf = "loss" /\ hold > 0 /\ Mutant # "nohold"
       THEN /\ hold' = hold - 1
            /\ deaf' = IF hold = 1 THEN SnrRule(TRUE, band) ELSE "loss"
       ELSE UNCHANGED <<hold, deaf>>
    /\ UNCHANGED <<band, reqs, fellBack, heard, lastMfsk>>

Next == \/ \E b \in Bands, mfsk \in BOOLEAN, answer \in BOOLEAN : Frame(b, mfsk, answer)
        \/ AskUnanswered
        \/ RoundUnanswered
        \/ Silence
        \/ Tick

\* Time always passes; nothing else is owed.
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick)

LossEnds == deaf = "loss" ~> deaf # "loss"

LossNeedsAsks ==
    [][deaf # "loss" /\ deaf' = "loss" => reqs' >= 2 /\ heard /\ band # "high"]_vars

HardNeedsOfdm ==
    [][deaf = "hard" /\ deaf' # "hard" => ~lastMfsk']_vars

MfskTxStays == [][deaf = "mfsktx" => deaf' = "mfsktx"]_vars
=============================================================================
