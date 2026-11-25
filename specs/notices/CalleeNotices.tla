---------------------------- MODULE CalleeNotices ----------------------------
(***************************************************************************)
(* What a callee tells its host about an incoming call -- PENDING,         *)
(* CANCELPENDING, CONNECTED -- as datalink_arq/arq_fsm.c and arq.c say it: *)
(* fsm_listening's RX_CALL (cb_notify_pending), fsm_accepting's ACCEPT-   *)
(* retry exhaustion, the ACCEPT fallback and its hold of the host         *)
(* (ARQ_TD_HOLDING, ARQ_ACCEPT_FALLBACK_HOLD_MS, release_fallback_hold),  *)
(* car_connect_callee, pending_hold and arrival_hold.                     *)
(*                                                                         *)
(* The callee's states:                                                    *)
(*   "idle"       LISTENING, no fallback                                   *)
(*   "accepting"  ACCEPTING: answering a CALL it decoded                   *)
(*   "fallback"   LISTENING after the ACCEPT retries ran out, still seeded *)
(*                for the caller (accept_fallback): a frame of its         *)
(*                completes the session                                    *)
(*   "connected"  CONNECTED                                                *)
(*   "off"        the host took the radio: LISTEN OFF, or its CONNECT      *)
(*                                                                         *)
(* held is the code's hold (teardown = ARQ_TD_HOLDING); inHold is time:    *)
(* within ARQ_ACCEPT_FALLBACK_HOLD_MS of our last ACCEPT, when a caller    *)
(* that took it may still come.  aPend is arrival_hold's own PENDING: a    *)
(* decoder locked on a frame while LISTENING, before it decodes (#527).   *)
(*                                                                         *)
(* The host's view is hp: a PENDING not yet ended by a CANCELPENDING or a *)
(* CONNECTED.  PENDING may repeat (pending_hold, every 20 s).  CONNECTED   *)
(* may come with no PENDING before it: a late connect after the hold, from *)
(* a frame no decoder locked on first (the 500 Hz MFSK is not watched).   *)
(*                                                                         *)
(* Time is abstract: the hold's end and the fallback's end are actions,   *)
(* the hold's always first.  The caller is not modelled: its frame, its   *)
(* DISCONNECT and a new CALL may come at any time they could.             *)
(*                                                                         *)
(* Properties (invariants):                                                *)
(*   NoStrayCancel -- no CANCELPENDING without a PENDING open: one end per *)
(*                    PENDING.                                             *)
(*   Settled       -- the host is held exactly while the callee answers   *)
(*                    (ACCEPTING), holds the fallback, or an arrival lock  *)
(*                    lasts.  No path leaves a PENDING open behind it.    *)
(*   LateHeld      -- in the fallback, within the hold's time, the host   *)
(*                    is held: a late connect finds it on the channel.    *)
(*   HeldInTime    -- the code holds only in the fallback, within its time.*)
(*                                                                         *)
(* Mutant:                                                                 *)
(*   "early"   CANCELPENDING as the retries run out (before #657).        *)
(*   "noguard" arrival_hold runs during the hold (#657 before 77bcbd4c):  *)
(*             a caller's poll that locks and does not decode ends in a   *)
(*             CANCELPENDING mid-hold.                                    *)
(*   "silent"  the host's DISCONNECT leaves a held fallback without its   *)
(*             CANCELPENDING.                                              *)
(***************************************************************************)
EXTENDS TLC

CONSTANT Mutant

VARIABLES st, held, inHold, aPend, hp, stray

vars == <<st, held, inHold, aPend, hp, stray>>

States == {"idle", "accepting", "fallback", "connected", "off"}
Listening == {"idle", "fallback"}

TypeOK ==
    /\ st \in States
    /\ held \in BOOLEAN
    /\ inHold \in BOOLEAN
    /\ aPend \in BOOLEAN
    /\ hp \in BOOLEAN
    /\ stray \in BOOLEAN

Init ==
    /\ st = "idle"
    /\ held = FALSE
    /\ inHold = FALSE
    /\ aPend = FALSE
    /\ hp = FALSE
    /\ stray = FALSE

(* The host notices. *)
Pending   == hp' = TRUE  /\ UNCHANGED stray
Cancel    == hp' = FALSE /\ stray' = (stray \/ ~hp)
Connected == hp' = FALSE /\ UNCHANGED stray
Quiet     == UNCHANGED <<hp, stray>>
(* The host's own LISTEN OFF or CONNECT: it has let the call go itself, so *)
(* an arrival hold ends with nothing said (arrival_hold, "!idle").        *)
HostLets  == hp' = FALSE /\ UNCHANGED stray

(* arrival_hold runs while LISTENING, not keyed, not holding the fallback. *)
ArrivalOn == st \in Listening /\ (~held \/ Mutant = "noguard")

(* -------------------------------------------------------------------- *)

(* fsm_listening RX_CALL: a CALL for us decodes, in LISTENING or the      *)
(* fallback (then handed over): ACCEPTING, cb_notify_pending.  An arrival *)
(* hold in progress is the session's now (arrival_hold resets).            *)
CallDecoded ==
    /\ st \in Listening
    /\ st' = "accepting"
    /\ held' = FALSE /\ inHold' = FALSE /\ aPend' = FALSE
    /\ Pending

(* arrival_hold: a decoder locks on a frame -- PENDING. *)
LockStart ==
    /\ ArrivalOn
    /\ ~aPend
    /\ aPend' = TRUE
    /\ Pending
    /\ UNCHANGED <<st, held, inHold>>

(* arrival_hold: the lock is gone and no CALL for us came of it (another *)
(* station's frame, noise, a caller's poll that did not decode).          *)
LockGone ==
    /\ ArrivalOn
    /\ aPend
    /\ aPend' = FALSE
    /\ Cancel
    /\ UNCHANGED <<st, held, inHold>>

(* fsm_accepting TIMER_RETRY, retries spent.  keyed: an ACCEPT went out   *)
(* (the seed is set): the fallback.  soon: within the hold's time of it.  *)
RetriesOut ==
    /\ st = "accepting"
    /\ \E keyed \in BOOLEAN, soon \in BOOLEAN :
         LET hold == keyed /\ soon /\ Mutant # "early" IN
         /\ st' = IF keyed THEN "fallback" ELSE "idle"
         /\ inHold' = (keyed /\ soon)
         /\ held' = hold
         /\ aPend' = FALSE
         /\ IF hold THEN Quiet ELSE Cancel

(* fsm_listening TIMER_RETRY at ARQ_ACCEPT_FALLBACK_HOLD_MS after our    *)
(* last ACCEPT: release_fallback_hold.                                    *)
HoldEnds ==
    /\ st = "fallback"
    /\ inHold
    /\ inHold' = FALSE
    /\ held' = FALSE
    /\ IF held THEN Cancel ELSE Quiet
    /\ UNCHANGED <<st, aPend>>

(* fsm_listening TIMER_RETRY at ARQ_CAR_PEER_LOST_MS: end_accept_fallback. *)
FallbackEnds ==
    /\ st = "fallback"
    /\ ~inHold
    /\ st' = "idle"
    /\ held' = FALSE
    /\ Quiet
    /\ UNCHANGED <<inHold, aPend>>

(* car_connect_callee: the caller's first frame, in ACCEPTING or the     *)
(* fallback.  CONNECTED.                                                  *)
CallerFrame ==
    /\ st \in {"accepting", "fallback"}
    /\ st' = "connected"
    /\ held' = FALSE /\ inHold' = FALSE /\ aPend' = FALSE
    /\ Connected

(* The caller's DISCONNECT for this call (fsm_accepting / fsm_listening  *)
(* RX_DISCONNECT), or the host's DISCONNECT or ABORT (APP_DISCONNECT).   *)
EndsCall(silent) ==
    /\ st \in {"accepting", "fallback"}
    /\ st' = "idle"
    /\ held' = FALSE /\ inHold' = FALSE
    /\ IF st = "accepting" \/ (held /\ ~silent) THEN Cancel ELSE Quiet
    /\ UNCHANGED aPend

CallerDisc == EndsCall(FALSE)
HostDisc   == EndsCall(Mutant = "silent")

(* LISTEN OFF (APP_STOP_LISTEN) or a host CONNECT (APP_CONNECT).         *)
HostTakes ==
    /\ st \in {"idle", "accepting", "fallback"}
    /\ st' = "off"
    /\ held' = FALSE /\ inHold' = FALSE /\ aPend' = FALSE
    /\ IF st = "accepting" \/ held THEN Cancel ELSE HostLets

ListenOn ==
    /\ st = "off"
    /\ st' = "idle"
    /\ Quiet
    /\ UNCHANGED <<held, inHold, aPend>>

SessionEnds ==
    /\ st = "connected"
    /\ st' = "idle"
    /\ Quiet
    /\ UNCHANGED <<held, inHold, aPend>>

Next ==
    \/ CallDecoded \/ LockStart \/ LockGone \/ RetriesOut \/ HoldEnds
    \/ FallbackEnds \/ CallerFrame \/ CallerDisc \/ HostDisc \/ HostTakes
    \/ ListenOn \/ SessionEnds

Spec == Init /\ [][Next]_vars

(* -------------------------------------------------------------------- *)

NoStrayCancel == ~stray

Settled == hp = (st = "accepting" \/ held \/ aPend)

LateHeld == (st = "fallback" /\ inHold) => hp

HeldInTime == held => (st = "fallback" /\ inHold)

=============================================================================
