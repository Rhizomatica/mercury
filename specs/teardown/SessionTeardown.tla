---------------------------- MODULE SessionTeardown ----------------------------
(***************************************************************************)
(* How a carousel session ends, as datalink_arq/arq_fsm.c runs it          *)
(* (car_begin_disconnect, fsm_disconnecting, the RX_DISCONNECT and RX_CALL *)
(* cases, the carousel watchdogs).                                        *)
(*                                                                         *)
(*   M, the caller, keys its DISCONNECT when its application asks (after  *)
(*   the drain: specs/carousel DirDone), and again up to R times while    *)
(*   nothing answers; then it gives up.  It keeps polling while connected,*)
(*   and each poll gives S a slot.                                        *)
(*   S, the callee, keys nothing of its own: its DISCONNECT goes in its   *)
(*   next slot, and in every slot after it while it is ending; with no    *)
(*   slot in ARQ_CAR_END_WAIT_MS it gives up.                             *)
(*   Either end, hearing the peer's DISCONNECT: connected, it replies one *)
(*   guard later and ends; ending, it ends -- replying first if its own   *)
(*   had not gone out yet (the crossed case).  Hearing the peer CALL      *)
(*   again while ending, it ends and takes the CALL.  Silence ends a      *)
(*   connected session (ARQ_CAR_PEER_LOST_MS).                           *)
(*   A DISCONNECT is taken on its session id (6 bits on the air) alone:   *)
(*   its CRC is plain.  Every other in-session frame is seeded with the   *)
(*   session's nonce, so only its own session decodes it.                 *)
(*                                                                         *)
(* Time is abstract: a timer fires only once nothing more is coming        *)
(* (the timers are far longer than an exchange) -- or, on a lossy channel,*)
(* whenever the queue to it is empty.  The channel delivers in order and  *)
(* may lose any frame.  The caller may call again (a deferred CONNECT)    *)
(* once it has ended, up to NS sessions.                                   *)
(*                                                                         *)
(* Properties:                                                             *)
(*   NoKill  -- no end leaves a session on another session's frame.       *)
(*   Clean   -- on a lossless channel no end ever times out: every        *)
(*              teardown is answered.                                     *)
(*   Mutual  -- once one end has finished a session, eventually neither   *)
(*              is in it.                                                  *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS NS,      \* sessions the caller may place
          SIDS,    \* session ids on the air
          R,       \* DISCONNECT retries (ARQ_DISCONNECT_RETRY_SLOTS)
          QMAX,    \* frames queued each way (bounds TLC)
          LOSS,    \* the channel may lose frames
          FOREIGN, \* another station's DISCONNECT may be heard
          Mutant   \* "none", or one rule broken:
                   \*   "crossedsilent": ending, the peer's DISCONNECT before mine
                   \*                    went out ends me without a reply

E == {"M", "S"}
Other(e) == IF e = "M" THEN "S" ELSE "M"

VARIABLES
    st,      \* [E -> {"idle", "calling", "conn", "ending", "reply"}]
             \*   "reply": ended, its reply to the peer's DISCONNECT still to key
    gen,     \* [E -> 0..NS]: the session it is (or last was) in -- the nonce
    sid,     \* [E -> SIDS \cup {0}]: that session's id on the air
    sent,    \* [E -> BOOLEAN]: its DISCONNECT went out (disconnect_sent)
    tries,   \* M's DISCONNECT retries left
    slot,    \* S holds a slot (heard an M poll since its last keydown)
    q,       \* [E -> Seq(frame)]: frames on their way to e
    placed,  \* sessions placed so far
    how      \* ghost: {[e, g, why]} -- how each end left each session

vars == <<st, gen, sid, sent, tries, slot, q, placed, how>>

Frame(k, s, g) == [k |-> k, sid |-> s, g |-> g]
Send(e, f) == Len(q[e]) < QMAX /\ q' = [q EXCEPT ![e] = Append(@, f)]     \* (TLC's bound: waits for room)
Pop(e) == q' = [q EXCEPT ![e] = Tail(@)]
End(e, why) == how' = how \cup {[e |-> e, g |-> gen[e], why |-> why]}

Init == /\ st = [e \in E |-> "idle"] /\ gen = [e \in E |-> 0] /\ sid = [e \in E |-> 0]
        /\ sent = [e \in E |-> FALSE] /\ tries = 0 /\ slot = FALSE
        /\ q = [e \in E |-> <<>>] /\ placed = 0 /\ how = {}

\* Something more will come to e from its peer, or is already on its way:
\* a peer in the same session talks while connected (M polls at least every
\* ~90 s, S answers), M keys its DISCONNECT, S keys its own in its slot.
Coming(e) == LET o == Other(e) IN
             \/ q[e] # <<>>
             \/ st[o] = "reply"
             \/ /\ gen[o] = gen[e]
                /\ \/ st[o] = "conn"
                   \/ st[o] = "ending" /\ o = "M" /\ ~sent["M"]
                   \/ st[o] = "ending" /\ o = "S" /\ (slot \/ st["M"] = "conn")
\* e's timer may fire: nothing more is coming; on a lossy channel, whenever
\* nothing is queued to it (what was lost is not coming).
TimerOK(e) == IF LOSS THEN q[e] = <<>> ELSE ~Coming(e) /\ q[Other(e)] = <<>>

\* ---- the caller ----
Call == /\ st["M"] = "idle" /\ placed < NS
        /\ \E s \in SIDS :
             /\ placed' = placed + 1
             /\ st' = [st EXCEPT !["M"] = "calling"] /\ gen' = [gen EXCEPT !["M"] = placed + 1]
             /\ sid' = [sid EXCEPT !["M"] = s] /\ sent' = [sent EXCEPT !["M"] = FALSE]
             /\ Send("S", Frame("call", s, placed + 1))
        /\ UNCHANGED <<tries, slot, how>>
CallFails == /\ st["M"] = "calling" /\ TimerOK("M")
             /\ st' = [st EXCEPT !["M"] = "idle"] /\ End("M", "nocall")
             /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed>>
Poll == /\ st["M"] = "conn"
        /\ Send("S", Frame("poll", sid["M"], gen["M"]))
        /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how>>
MAppDisc == /\ st["M"] = "conn"
            /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
            /\ tries' = R
            /\ UNCHANGED <<gen, sid, slot, q, placed, how>>
MDisc == /\ st["M"] = "ending" /\ ~sent["M"]
         /\ sent' = [sent EXCEPT !["M"] = TRUE]
         /\ Send("S", Frame("disc", sid["M"], gen["M"]))
         /\ UNCHANGED <<st, gen, sid, tries, slot, placed, how>>
MRetry == /\ st["M"] = "ending" /\ sent["M"] /\ tries > 0 /\ TimerOK("M")
          /\ tries' = tries - 1
          /\ Send("S", Frame("disc", sid["M"], gen["M"]))
          /\ UNCHANGED <<st, gen, sid, sent, slot, placed, how>>
MGiveUp == /\ st["M"] = "ending" /\ sent["M"] /\ tries = 0 /\ TimerOK("M")
           /\ st' = [st EXCEPT !["M"] = "idle"] /\ End("M", "timeout")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed>>

\* ---- the callee ----
SAppDisc == /\ st["S"] = "conn"
            /\ st' = [st EXCEPT !["S"] = "ending"] /\ sent' = [sent EXCEPT !["S"] = FALSE]
            /\ UNCHANGED <<gen, sid, tries, slot, q, placed, how>>
SSlotDisc == /\ st["S"] = "ending" /\ slot
             /\ slot' = FALSE /\ sent' = [sent EXCEPT !["S"] = TRUE]
             /\ Send("M", Frame("disc", sid["S"], gen["S"]))
             /\ UNCHANGED <<st, gen, sid, tries, placed, how>>
SGiveUp == /\ st["S"] = "ending" /\ TimerOK("S")
           /\ st' = [st EXCEPT !["S"] = "idle"] /\ End("S", "timeout")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed>>

\* ---- either end ----
Lost(e) == /\ st[e] = "conn" /\ TimerOK(e)
           /\ st' = [st EXCEPT ![e] = "idle"] /\ End(e, "lost")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed>>
Reply(e) == /\ st[e] = "reply"
            /\ st' = [st EXCEPT ![e] = "idle"]
            /\ Send(Other(e), Frame("disc", sid[e], gen[e]))
            /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, how>>

\* e takes the frame at the head of its queue.
Mine(e, f) == f.g = gen[e]                                   \* seeded: its session's
HearDisc(e, f) ==
    /\ f.k = "disc" /\ st[e] \in {"conn", "ending"}
    /\ f.sid = sid[e]
    /\ LET why == IF f.g = gen[e] THEN "peer" ELSE "killed" IN
       IF st[e] = "ending" /\ (sent[e] \/ Mutant = "crossedsilent")
       THEN st' = [st EXCEPT ![e] = "idle"] /\ End(e, why)
       ELSE st' = [st EXCEPT ![e] = "reply"] /\ End(e, why)
    /\ UNCHANGED <<gen, sid, sent, tries, slot, placed>>
Receive(e) ==
    /\ q[e] # <<>>
    /\ LET f == Head(q[e]) IN
       \/ HearDisc(e, f) /\ Pop(e)
       \* S answers a CALL when idle, or when ending and its peer calls again
       \/ /\ e = "S" /\ f.k = "call" /\ st["S"] \in {"idle", "ending"}
          /\ st' = [st EXCEPT !["S"] = "conn"] /\ gen' = [gen EXCEPT !["S"] = f.g]
          /\ sid' = [sid EXCEPT !["S"] = f.sid] /\ sent' = [sent EXCEPT !["S"] = FALSE]
          /\ slot' = FALSE
          /\ IF st["S"] = "ending" THEN End("S", "recall") ELSE UNCHANGED how
          /\ Len(q["M"]) < QMAX
          /\ q' = [q EXCEPT !["S"] = Tail(@), !["M"] = Append(@, Frame("accept", f.sid, f.g))]
          /\ UNCHANGED <<tries, placed>>
       \/ /\ e = "M" /\ f.k = "accept" /\ st["M"] = "calling" /\ Mine("M", f)
          /\ st' = [st EXCEPT !["M"] = "conn"] /\ Pop("M")
          /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, how>>
       \/ /\ e = "S" /\ f.k = "poll" /\ st["S"] \in {"conn", "ending"} /\ Mine("S", f)
          /\ slot' = TRUE /\ Pop("S")
          /\ UNCHANGED <<st, gen, sid, sent, tries, placed, how>>
       \* anything else: not for it, or not in its state -- dropped
       \/ /\ ~(f.k = "disc" /\ st[e] \in {"conn", "ending"} /\ f.sid = sid[e])
          /\ ~(e = "S" /\ f.k = "call" /\ st["S"] \in {"idle", "ending"})
          /\ ~(e = "M" /\ f.k = "accept" /\ st["M"] = "calling" /\ Mine("M", f))
          /\ ~(e = "S" /\ f.k = "poll" /\ st["S"] \in {"conn", "ending"} /\ Mine("S", f))
          /\ Pop(e)
          /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how>>

\* ---- the channel ----
Lose(e) == LOSS /\ q[e] # <<>> /\ Pop(e) /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how>>
Foreign(e) == /\ FOREIGN /\ \E s \in SIDS : Send(e, Frame("disc", s, 0))
              /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how>>

\* All sessions placed and over: TLC stops here.
Done == placed = NS /\ \A e \in E : st[e] = "idle" /\ q[e] = <<>>
        /\ UNCHANGED vars

Next == \/ Done \/ Call \/ CallFails \/ Poll \/ MAppDisc \/ MDisc \/ MRetry \/ MGiveUp
        \/ SAppDisc \/ SSlotDisc \/ SGiveUp
        \/ \E e \in E : Lost(e) \/ Reply(e) \/ Receive(e) \/ Lose(e) \/ Foreign(e)

\* Progress is fair; the application, the channel's losses and other
\* stations are not.
Fair == /\ WF_vars(MDisc) /\ WF_vars(MRetry) /\ WF_vars(MGiveUp) /\ WF_vars(CallFails)
        /\ WF_vars(SSlotDisc) /\ WF_vars(SGiveUp) /\ WF_vars(Poll)
        /\ \A e \in E : WF_vars(Reply(e)) /\ WF_vars(Receive(e)) /\ WF_vars(Lost(e))
Spec == Init /\ [][Next]_vars /\ Fair

\* ---- properties ----
TypeOK == /\ st \in [E -> {"idle", "calling", "conn", "ending", "reply"}]
          /\ tries \in 0 .. R /\ placed \in 0 .. NS
NoKill == \A h \in how : h.why # "killed"
Clean == ~LOSS => \A h \in how : h.why \in {"peer", "recall"}
In(e, g) == gen[e] = g /\ st[e] \in {"calling", "conn", "ending", "reply"}
Mutual == \A g \in 1 .. NS :
            (\E h \in how : h.g = g /\ h.why # "nocall") ~> ~(\E e \in E : In(e, g))
=============================================================================
