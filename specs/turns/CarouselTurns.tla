---------------------------- MODULE CarouselTurns -----------------------------
(***************************************************************************)
(* Who may key, and when: the turn rules of docs/CAROUSEL-TURNS.md, in    *)
(* discrete time.  Two half-duplex stations share one channel.            *)
(*                                                                         *)
(*   M, the caller, is the session's only timing master.  It may key a    *)
(*   keydown of any length at any time, as long as the keydown (with a     *)
(*   guard either side) misses every interval in which S could be on the  *)
(*   air.  M computes those intervals from its own keydowns' end times,   *)
(*   which it knows exactly; it never relies on hearing or sensing S.     *)
(*                                                                         *)
(*   S, the callee, keys only in slots anchored to the end of an M        *)
(*   keydown it decoded: the response slot (Tg after that end) and up to  *)
(*   K continuation slots after it, each P(r) later.  It learns the end   *)
(*   only by decoding, at most D after it (late decodes are not answered),*)
(*   and anchors to the time it decoded, so its slots start up to D late. *)
(*   Each S keydown is at most r long, the length M's keydown allowed.   *)
(*   A newer decoded M keydown replaces the anchor.                       *)
(*                                                                         *)
(* The channel loses any M keydown (S does not decode it); what S sends   *)
(* is irrelevant to safety, because M never acts on it for timing.        *)
(*                                                                         *)
(* Safety: the two are never on the air at the same time.                 *)
(*                                                                         *)
(* With CLOSE, what lets M take a slot as over (m_close_slot,             *)
(* m_close_from, m_close_spent, pattern_in_time in carousel.c):           *)
(*   - S's keydown is a round (its frames name the M keydown and slot),   *)
(*     a status saying it has nothing (in a response slot), or a pattern  *)
(*     (no name; in a response slot).  S continues in a later slot only   *)
(*     after a round of its own on that anchor.                           *)
(*   - M, hearing a round, closes its slot; a status, every slot of that  *)
(*     keydown; either way, every slot of every older keydown too (S      *)
(*     re-anchors on each M keydown it decodes).                          *)
(*   - M detects a pattern up to DP after it ends; it takes it -- as the  *)
(*     answer to its latest keydown, closing slot 0 and older keydowns --  *)
(*     only off the air, and only in time: no sooner than Tg + PMIN after *)
(*     that keydown, no later than DP past its response slot.             *)
(* With DISC, M also keys the FSM's DISCONNECT, which S answers TgD after *)
(* it decodes it (one slot), and logs it (car_log_ctl).  With FALSEPAT,   *)
(* M's pattern detector may false-alarm.  With ONE, an M keydown may be a *)
(* poll for one frame (an idle ask): it has the response slot only, no    *)
(* continuations, at both ends -- each reads it from the keydown itself. *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS Tg,    \* turnaround: S's slot opens this long after the end it anchors to
          D,     \* the latest S decodes (and so learns the end of) an M keydown
          G,     \* M's guard either side of a slot
          K,     \* continuation slots after the response slot
          A,     \* continuation spacing: the gap after an S keydown's latest end
          ML,    \* the lengths M's keydowns may have
          RL,    \* the S keydown lengths an M keydown may allow
          H,     \* horizon (time bound for TLC)
          CLOSE, \* the slot-closing rules (above)
          DP,    \* the latest M detects a pattern after it ends
          PMIN,  \* pattern_in_time: no sooner than Tg + PMIN after M's keydown
          DISC,  \* M also keys DISCONNECTs
          TgD,   \* ...which S answers this long after it decodes one
          FALSEPAT, \* M's pattern detector may false-alarm
          ONE,   \* M keys one-frame polls, which have no continuation slots
          Mutant \* "none", or one rule broken (to show the model can fail):
                 \*   "nogrid": M minds only the response slot, not continuations
                 \*   "late":   S answers a decode up to D+1 late
                 \*   "long":   S keys one unit longer than allowed
                 \*   "blind":  S also keys on a timer of its own (today's design)
                 \*   "reuse":  S keys twice in one slot (M took it as over)
                 \*   with CLOSE / DISC:
                 \*   "spentany":  S continues after a status of nothing
                 \*   "anytime":   M takes a pattern whenever it hears one
                 \*   "closenewer": M closes newer keydowns' slots too, not only older
                 \*   "nologdisc": M does not log its DISCONNECT
                 \*   with ONE:
                 \*   "onecont":  S continues after a one-frame poll (M keeps
                 \*               no continuation slots for it)

ASSUME Tg \in Nat /\ D \in Nat /\ G \in Nat /\ K \in Nat /\ A \in Nat
ASSUME ML \subseteq Nat \ {0} /\ RL \subseteq Nat \ {0}
ASSUME CLOSE \in BOOLEAN /\ DISC \in BOOLEAN /\ FALSEPAT \in BOOLEAN /\ ONE \in BOOLEAN
ASSUME DP \in Nat /\ PMIN \in Nat /\ TgD \in Nat

VARIABLES
    t,         \* now
    mOn,       \* M on the air
    mEnd,      \* ...until then
    mR,        \* ...allowing S keydowns this long
    mDc,       \* ...a DISCONNECT
    mOne,      \* ...a one-frame poll
    mLog,      \* M's recent keydowns: [e: end, r: allowed length, dc: a DISCONNECT,
               \*   one: a one-frame poll]
    inflight,  \* M keydowns S may still decode: [e, r, dc, one]
    sOn,       \* S on the air
    sEnd,      \* ...until then
    sSlot,     \* ...in this slot: [e: the M keydown it answers, i, dc]
    sKind,     \* ...as "round", "status" (nothing to send) or "pattern"
    anchor,    \* S's slots: [e: which M keydown, eh: its end as S learned it,
               \*   r, i: next slot, dc, one, sr: S sent a round on it] or NoAnchor
    pPend,     \* the end of S's pattern M may still detect, or -1
    mDone      \* slots M heard S's keydown in, end to end: S keys once per
               \* slot, so they are over (S's frames name keydown and slot)

SetMax(S) == CHOOSE x \in S : \A y \in S : y <= x
SetMin(S) == CHOOSE x \in S : \A y \in S : x <= y

vars == <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor, pPend, mDone>>
NoAnchor == [e |-> 0, eh |-> 0, r |-> 0, i |-> -1, dc |-> FALSE, one |-> FALSE, sr |-> FALSE]

P(r) == r + A                                   \* slot period
Gt(dc) == IF dc THEN TgD ELSE Tg                \* turnaround
\* A DISCONNECT has one slot, and so has a one-frame poll: M's count...
Kof(k) == IF k.dc \/ k.one THEN 0 ELSE K
\* ...and S's
KofS(a) == IF a.dc \/ (a.one /\ Mutant # "onecont") THEN 0 ELSE K
\* Slot i of an M keydown that ended at e, allowing r: S starts in
\* [e + Tg + i*P(r), e + Tg + D + i*P(r)] and is done r later at most.
SLo(k, i) == k.e + Gt(k.dc) + i * P(k.r)
SHi(k, i) == k.e + Gt(k.dc) + D + i * P(k.r) + k.r
\* [a, b) and [c, d) meet
Meet(a, b, c, d) == a < d /\ c < b

\* M's only rule: [s - G, s + L + G) misses every slot of every keydown in
\* its log that it has not heard S finish.
FreeFor(s, L) ==
    \A k \in mLog : \A i \in 0 .. Kof(k) :
        \/ (Mutant = "nogrid" /\ i > 0)
        \/ [e |-> k.e, i |-> i] \in mDone
        \/ ~Meet(s - G, s + L + G, SLo(k, i), SHi(k, i))

\* A keydown's slots are all over: M forgets it.
Expired(k) == t >= SHi(k, Kof(k)) + G

\* M closes: every slot of every keydown older than the one S answered
\* (m_close_from), and every slot of a keydown S said it has nothing for.
Older(e) == IF CLOSE THEN {[e |-> k.e, i |-> j] : k \in {x \in mLog : x.e < e \/ (Mutant = "closenewer" /\ x.e # e)},
                                                  j \in 0 .. K} ELSE {}
AllOf(e) == {[e |-> e, i |-> j] : j \in 0 .. K}
\* M's latest keydown, which a pattern is taken to answer
Latest == CHOOSE k \in mLog : \A x \in mLog : x.e <= k.e
InTime == mLog # {} /\ (Mutant = "anytime" \/
          (t >= Latest.e + Tg + PMIN /\ t <= SHi(Latest, 0) + DP))
TakePattern == mDone' = mDone \cup {[e |-> Latest.e, i |-> 0]} \cup Older(Latest.e)

Init ==
    /\ t = 0 /\ mOn = FALSE /\ mEnd = 0 /\ mR = 0 /\ mDc = FALSE /\ mOne = FALSE /\ mLog = {}
    /\ inflight = {} /\ sOn = FALSE /\ sEnd = 0 /\ anchor = NoAnchor
    /\ sSlot = [e |-> 0, i |-> -1, dc |-> FALSE] /\ sKind = "round" /\ pPend = -1 /\ mDone = {}

\* ---- M ----
\* (a DISCONNECT as any other: car_disconnect_must_wait keys it free)
MStart(L, R, dc, one) ==
    /\ ~mOn
    /\ FreeFor(t, L)
    /\ t + L <= H
    /\ mOn' = TRUE /\ mEnd' = t + L /\ mR' = R /\ mDc' = dc /\ mOne' = one
    /\ UNCHANGED <<t, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor, pPend, mDone>>

MStop ==
    /\ mOn /\ t = mEnd
    /\ mOn' = FALSE
    /\ mLog' = {k \in mLog : ~Expired(k)} \cup
              (IF mDc /\ Mutant = "nologdisc" THEN {} ELSE {[e |-> t, r |-> mR, dc |-> mDc, one |-> mOne]})
    /\ mDone' = {d \in mDone : \E k \in mLog' : k.e = d.e}
    /\ inflight' = inflight \cup {[e |-> t, r |-> mR, dc |-> mDc, one |-> mOne]}
    /\ UNCHANGED <<t, mEnd, mR, mDc, mOne, sOn, sEnd, sSlot, sKind, anchor, pPend>>

\* ---- S ----
\* Decoding an M keydown, no later than D after it ended (a frame decoded later
\* is still data, but not answered).
SDecode(k) ==
    /\ k \in inflight /\ t >= k.e /\ t <= k.e + D + (IF Mutant = "late" THEN 1 ELSE 0)
    /\ anchor' = [e |-> k.e, eh |-> t, r |-> k.r, i |-> 0, dc |-> k.dc, one |-> k.one, sr |-> FALSE]
    /\ inflight' = inflight \ {k}
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, sOn, sEnd, sSlot, sKind, pPend, mDone>>

SLose(k) ==
    /\ k \in inflight
    /\ inflight' = inflight \ {k}
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, sOn, sEnd, sSlot, sKind, anchor, pPend, mDone>>

SlotNow == anchor.i >= 0 /\ t = anchor.eh + Gt(anchor.dc) + anchor.i * P(anchor.r)
NextOf(a) == IF a.i + 1 > KofS(a) THEN NoAnchor ELSE [a EXCEPT !.i = @ + 1]
NextSlot == NextOf(anchor)
\* What S may key in this slot: anything in a response slot; in a later one,
\* with CLOSE, only more of a round it sent on this anchor (s_slot: sent_round).
Kinds == IF ~CLOSE THEN {"round"}
         ELSE IF anchor.i = 0 /\ ~anchor.dc THEN {"round", "status", "pattern"}
         ELSE IF anchor.sr \/ anchor.dc \/ Mutant = "spentany" THEN {"round"} ELSE {}

\* In a slot S sends anything up to r long -- or nothing (no data, or a
\* continuation it does not need).
SStart(r, kind) ==
    /\ ~sOn /\ SlotNow /\ ~(mOn /\ t = mEnd)
    /\ kind \in Kinds
    /\ r \in 1 .. anchor.r + (IF Mutant = "long" THEN 1 ELSE 0)
    /\ t + r <= H
    /\ sOn' = TRUE /\ sEnd' = t + r /\ sKind' = kind
    /\ sSlot' = [e |-> anchor.e, i |-> anchor.i, dc |-> anchor.dc]
    /\ anchor' = NextOf([anchor EXCEPT !.sr = @ \/ (CLOSE /\ kind = "round")])
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, pPend, mDone>>

\* "blind": S keys after Tg + D + A of silence since its last keydown, as
\* today's callee repeats a handover or nudges a silent peer.
SBlind ==
    /\ Mutant = "blind" /\ ~sOn /\ ~(mOn /\ t = mEnd) /\ anchor.i < 0
    /\ t = sEnd + Tg + D + A /\ t + 1 <= H
    /\ sOn' = TRUE /\ sEnd' = t + 1
    /\ sSlot' = [e |-> 0, i |-> -1, dc |-> FALSE]
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sKind, anchor, pPend, mDone>>

\* "reuse": S keys again in the slot it just finished.
SReuse ==
    /\ Mutant = "reuse" /\ ~sOn /\ sSlot.i >= 0 /\ t = sEnd /\ t + 1 <= H
    /\ ~(mOn /\ t = mEnd)
    /\ sOn' = TRUE /\ sEnd' = t + 1
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sSlot, sKind, anchor, pPend, mDone>>

SSkip ==
    /\ SlotNow
    /\ anchor' = NextSlot
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, pPend, mDone>>

\* S's keydown ends; M may hear it (at the earliest: as it ends) and so
\* learn that slot is over.  The answer to a DISCONNECT is the FSM's: the
\* carousel closes nothing on it.  A pattern M may detect later, or not.
Named == [e |-> sSlot.e, i |-> sSlot.i]
SStop ==
    /\ sOn /\ t = sEnd
    /\ sOn' = FALSE
    /\ \/ UNCHANGED <<mDone, pPend>>
       \/ /\ sSlot.i >= 0 /\ ~sSlot.dc /\ sKind = "round"
          /\ mDone' = mDone \cup {Named} \cup Older(sSlot.e) /\ UNCHANGED pPend
       \/ /\ sSlot.i >= 0 /\ ~sSlot.dc /\ sKind = "status"
          /\ mDone' = mDone \cup AllOf(sSlot.e) \cup Older(sSlot.e) /\ UNCHANGED pPend
       \/ sKind = "pattern" /\ pPend' = t /\ UNCHANGED mDone
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sEnd, sSlot, sKind, anchor>>

\* M's detector fires on S's pattern (car_on_pattern): ignored on the air or
\* out of time.
PatHear ==
    /\ pPend >= 0 /\ t >= pPend /\ pPend' = -1
    /\ IF ~mOn /\ InTime THEN TakePattern ELSE UNCHANGED mDone
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor>>
PatMiss ==
    /\ pPend >= 0 /\ pPend' = -1
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor, mDone>>
\* ...or on nothing at all
PatFalse ==
    /\ FALSEPAT /\ ~mOn /\ InTime /\ TakePattern
    /\ UNCHANGED <<t, mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor, pPend>>

\* ---- time ----
\* Time moves on once everything due now has happened.
Tick ==
    /\ t < H
    /\ ~(mOn /\ t = mEnd) /\ ~(sOn /\ t = sEnd) /\ ~SlotNow
    /\ \A k \in inflight : t < k.e + D + (IF Mutant = "late" THEN 1 ELSE 0)
    /\ ~(Mutant = "blind" /\ ~sOn /\ anchor.i < 0 /\ t = sEnd + Tg + D + A)
    /\ ~(pPend >= 0 /\ t >= pPend + DP)
    /\ t' = t + 1
    /\ UNCHANGED <<mOn, mEnd, mR, mDc, mOne, mLog, inflight, sOn, sEnd, sSlot, sKind, anchor, pPend, mDone>>

\* The horizon: TLC stops here (a deadlock before it is a real one).
Done == t = H /\ UNCHANGED vars

Next ==
    \/ Done
    \/ \E L \in ML, R \in RL, dc \in (IF DISC THEN BOOLEAN ELSE {FALSE}),
          one \in (IF ONE THEN BOOLEAN ELSE {FALSE}) : MStart(L, R, dc, one)
    \/ MStop \/ SStop \/ SSkip \/ Tick
    \/ \E r \in RL \cup {SetMax(RL) + 1}, kind \in {"round", "status", "pattern"} : SStart(r, kind)
    \/ SBlind \/ SReuse
    \/ PatHear \/ PatMiss \/ PatFalse
    \/ \E k \in inflight : SDecode(k) \/ SLose(k)

Spec == Init /\ [][Next]_vars

\* ---- properties ----
NoOverlap == ~(mOn /\ sOn)
TypeOK == /\ t \in 0 .. H /\ mOn \in BOOLEAN /\ sOn \in BOOLEAN
          /\ anchor.i \in -1 .. K
\* M is never shut out: off the air, it has a time to key its shortest
\* keydown within one whole slot pattern from now (whatever S does).
W == SetMax({Tg, TgD}) + D + K * P(SetMax(RL)) + SetMax(RL) + 2 * G + SetMin(ML)
MNotShutOut == ~mOn => \E s \in t .. t + W : FreeFor(s, SetMin(ML))
=============================================================================
