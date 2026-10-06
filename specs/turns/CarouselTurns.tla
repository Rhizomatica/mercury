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
          Mutant \* "none", or one rule broken (to show the model can fail):
                 \*   "nogrid": M minds only the response slot, not continuations
                 \*   "late":   S answers a decode up to D+1 late
                 \*   "long":   S keys one unit longer than allowed
                 \*   "blind":  S also keys on a timer of its own (today's design)
                 \*   "reuse":  S keys twice in one slot (M took it as over)

ASSUME Tg \in Nat /\ D \in Nat /\ G \in Nat /\ K \in Nat /\ A \in Nat
ASSUME ML \subseteq Nat \ {0} /\ RL \subseteq Nat \ {0}

VARIABLES
    t,         \* now
    mOn,       \* M on the air
    mEnd,      \* ...until then
    mR,        \* ...allowing S keydowns this long
    mLog,      \* M's recent keydowns: [e: end, r: allowed length]
    inflight,  \* M keydowns S may still decode: [e, r]
    sOn,       \* S on the air
    sEnd,      \* ...until then
    sSlot,     \* ...in this slot: [e: the M keydown it answers, i]
    anchor,    \* S's slots: [e: which M keydown, eh: its end as S learned it,
               \*   r, i: next slot] or NoAnchor
    mDone      \* slots M heard S's keydown in, end to end: S keys once per
               \* slot, so they are over (S's frames name keydown and slot)

SetMax(S) == CHOOSE x \in S : \A y \in S : y <= x
SetMin(S) == CHOOSE x \in S : \A y \in S : x <= y

vars == <<t, mOn, mEnd, mR, mLog, inflight, sOn, sEnd, sSlot, anchor, mDone>>
NoAnchor == [e |-> 0, eh |-> 0, r |-> 0, i |-> -1]

P(r) == r + A                                   \* slot period
\* Slot i of an M keydown that ended at e, allowing r: S starts in
\* [e + Tg + i*P(r), e + Tg + D + i*P(r)] and is done r later at most.
SLo(e, r, i) == e + Tg + i * P(r)
SHi(e, r, i) == e + Tg + D + i * P(r) + r
\* [a, b) and [c, d) meet
Meet(a, b, c, d) == a < d /\ c < b

\* M's only rule: [s - G, s + L + G) misses every slot of every keydown in
\* its log that it has not heard S finish.
FreeFor(s, L) ==
    \A k \in mLog : \A i \in 0 .. K :
        \/ (Mutant = "nogrid" /\ i > 0)
        \/ [e |-> k.e, i |-> i] \in mDone
        \/ ~Meet(s - G, s + L + G, SLo(k.e, k.r, i), SHi(k.e, k.r, i))

\* A keydown's slots are all over: M forgets it.
Expired(k) == t >= SHi(k.e, k.r, K) + G

Init ==
    /\ t = 0 /\ mOn = FALSE /\ mEnd = 0 /\ mR = 0 /\ mLog = {}
    /\ inflight = {} /\ sOn = FALSE /\ sEnd = 0 /\ anchor = NoAnchor
    /\ sSlot = [e |-> 0, i |-> -1] /\ mDone = {}

\* ---- M ----
MStart(L, R) ==
    /\ ~mOn
    /\ FreeFor(t, L)
    /\ t + L <= H
    /\ mOn' = TRUE /\ mEnd' = t + L /\ mR' = R
    /\ UNCHANGED <<t, mLog, inflight, sOn, sEnd, sSlot, anchor, mDone>>

MStop ==
    /\ mOn /\ t = mEnd
    /\ mOn' = FALSE
    /\ mLog' = {k \in mLog : ~Expired(k)} \cup {[e |-> t, r |-> mR]}
    /\ mDone' = {d \in mDone : \E k \in mLog' : k.e = d.e}
    /\ inflight' = inflight \cup {[e |-> t, r |-> mR]}
    /\ UNCHANGED <<t, mEnd, mR, sOn, sEnd, sSlot, anchor>>

\* ---- S ----
\* Decoding an M keydown, no later than D after it ended (a frame decoded later
\* is still data, but not answered).
SDecode(k) ==
    /\ k \in inflight /\ t >= k.e /\ t <= k.e + D + (IF Mutant = "late" THEN 1 ELSE 0)
    /\ anchor' = [e |-> k.e, eh |-> t, r |-> k.r, i |-> 0]
    /\ inflight' = inflight \ {k}
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, sOn, sEnd, sSlot, mDone>>

SLose(k) ==
    /\ k \in inflight
    /\ inflight' = inflight \ {k}
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, sOn, sEnd, sSlot, anchor, mDone>>

SlotNow == anchor.i >= 0 /\ t = anchor.eh + Tg + anchor.i * P(anchor.r)
NextSlot == IF anchor.i + 1 > K THEN NoAnchor ELSE [anchor EXCEPT !.i = @ + 1]

\* In a slot S sends anything up to r long -- or nothing (no data, or a
\* continuation it does not need).
SStart(r) ==
    /\ ~sOn /\ SlotNow /\ ~(mOn /\ t = mEnd)
    /\ r \in 1 .. anchor.r + (IF Mutant = "long" THEN 1 ELSE 0)
    /\ t + r <= H
    /\ sOn' = TRUE /\ sEnd' = t + r
    /\ sSlot' = [e |-> anchor.e, i |-> anchor.i]
    /\ anchor' = NextSlot
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, inflight, mDone>>

\* "blind": S keys after Tg + D + A of silence since its last keydown, as
\* today's callee repeats a handover or nudges a silent peer.
SBlind ==
    /\ Mutant = "blind" /\ ~sOn /\ ~(mOn /\ t = mEnd) /\ anchor.i < 0
    /\ t = sEnd + Tg + D + A /\ t + 1 <= H
    /\ sOn' = TRUE /\ sEnd' = t + 1
    /\ sSlot' = [e |-> 0, i |-> -1]
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, inflight, anchor, mDone>>

\* "reuse": S keys again in the slot it just finished.
SReuse ==
    /\ Mutant = "reuse" /\ ~sOn /\ sSlot.i >= 0 /\ t = sEnd /\ t + 1 <= H
    /\ ~(mOn /\ t = mEnd)
    /\ sOn' = TRUE /\ sEnd' = t + 1
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, inflight, sSlot, anchor, mDone>>

SSkip ==
    /\ SlotNow
    /\ anchor' = NextSlot
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, inflight, sOn, sEnd, sSlot, mDone>>

\* S's keydown ends; M may hear it (at the earliest: as it ends) and so
\* learn that slot is over.
SStop ==
    /\ sOn /\ t = sEnd
    /\ sOn' = FALSE
    /\ \/ mDone' = mDone
       \/ sSlot.i >= 0 /\ mDone' = mDone \cup {sSlot}
    /\ UNCHANGED <<t, mOn, mEnd, mR, mLog, inflight, sEnd, sSlot, anchor>>

\* ---- time ----
\* Time moves on once everything due now has happened.
Tick ==
    /\ t < H
    /\ ~(mOn /\ t = mEnd) /\ ~(sOn /\ t = sEnd) /\ ~SlotNow
    /\ \A k \in inflight : t < k.e + D + (IF Mutant = "late" THEN 1 ELSE 0)
    /\ ~(Mutant = "blind" /\ ~sOn /\ anchor.i < 0 /\ t = sEnd + Tg + D + A)
    /\ t' = t + 1
    /\ UNCHANGED <<mOn, mEnd, mR, mLog, inflight, sOn, sEnd, sSlot, anchor, mDone>>

\* The horizon: TLC stops here (a deadlock before it is a real one).
Done == t = H /\ UNCHANGED vars

Next ==
    \/ Done
    \/ \E L \in ML, R \in RL : MStart(L, R)
    \/ MStop \/ SStop \/ SSkip \/ Tick
    \/ \E r \in RL \cup {SetMax(RL) + 1} : SStart(r)
    \/ SBlind \/ SReuse
    \/ \E k \in inflight : SDecode(k) \/ SLose(k)

Spec == Init /\ [][Next]_vars

\* ---- properties ----
NoOverlap == ~(mOn /\ sOn)
TypeOK == /\ t \in 0 .. H /\ mOn \in BOOLEAN /\ sOn \in BOOLEAN
          /\ anchor.i \in -1 .. K
\* M is never shut out: off the air, it has a time to key its shortest
\* keydown within one whole slot pattern from now (whatever S does).
W == Tg + D + K * P(SetMax(RL)) + SetMax(RL) + 2 * G + SetMin(ML)
MNotShutOut == ~mOn => \E s \in t .. t + W : FreeFor(s, SetMin(ML))
=============================================================================
