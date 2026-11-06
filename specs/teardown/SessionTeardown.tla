---------------------------- MODULE SessionTeardown ----------------------------
(***************************************************************************)
(* How a carousel session ends, as datalink_arq/arq_fsm.c runs it          *)
(* (car_begin_disconnect, fsm_disconnecting, the RX_DISCONNECT and RX_CALL *)
(* cases, the carousel watchdogs).                                        *)
(*                                                                         *)
(*   M, the caller, keys its DISCONNECT when its application asks, and   *)
(*   again up to R times while nothing answers; then it gives up.  It    *)
(*   keeps polling while connected, and each poll gives S a slot.  With   *)
(*   its last word not yet confirmed, M holds the DISCONNECT while its    *)
(*   drain delivers it (ARQ_TD_DRAINING; how, specs/carousel DirDone),   *)
(*   up to a deadline or the peer's silence.  The host, told DISCONNECTED *)
(*   already, may ask again meanwhile -- a second DISCONNECT, or a CONNECT *)
(*   -- and either ends the drain at once, dropping what it holds.        *)
(*   Once S has said M_END, M remembers it; if S then falls silent as    *)
(*   long as S's own drain would last, M ends with one DISCONNECT.       *)
(*   S, the callee, keys nothing of its own: its DISCONNECT goes in its   *)
(*   next slot, and in every slot after it while it is ending; with no    *)
(*   slot in ARQ_CAR_END_WAIT_MS it gives up.  While its DISCONNECT waits *)
(*   for its data to drain, its answers in its slots say its application *)
(*   has hung up (M_END, poll type 3); M, connected, hearing one of its   *)
(*   session and holding all S has, ends the session as if its own       *)
(*   application had asked.  S may hang up with a last word not yet with  *)
(*   M; it reaches M while both are in the session.  M's last word (B2F's *)
(*   FQ) reaches S, which owes an answer acknowledging it; S hanging up   *)
(*   owing it answers first, saying so (M_END), not with its DISCONNECT. *)
(*   Either end, hearing the peer's DISCONNECT: connected, it replies one *)
(*   guard later and ends; ending, it ends -- replying first if its own   *)
(*   had not gone out yet (the crossed case).  Hearing its peer CALL     *)
(*   again while ending, it ends and takes the CALL; a callee whose      *)
(*   DISCONNECT is out takes a CALL from any callsign (a station calls    *)
(*   with another MYCALL per application).  Silence ends a connected      *)
(*   session (ARQ_CAR_PEER_LOST_MS).                                       *)
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
(*   Answered -- a callee that is idle, or ending with its DISCONNECT out *)
(*              (waiting only for the answer), never drops a CALL for it.  *)
(*   NoUnasked -- M ends a session on an M_END only of that session.      *)
(*   NoLostWord -- M never ends on an M_END while S holds a last word M   *)
(*              has not got (the application wrote it, then hung up).     *)
(*              Not M ending on S's silence after one: that word is lost  *)
(*              to the channel, as at the watchdog (ghost wordgone).      *)
(*   NoCut   -- M ends a session on silence only once S's application    *)
(*              has hung up in it (N3JXB -> KG9NI-1, 2026-11-05).        *)
(*   NoInherited -- S holds a DISCONNECT back only for the session whose  *)
(*              application hung up: no teardown step outlives its        *)
(*              session (sess_enter resets arq_session_t.teardown).       *)
(*   Confirmed -- on a lossless channel M never leaves a session with its *)
(*              last word unacknowledged (car_owes_ack, 2026-11-05).      *)
(*              Not when the host had it dropped (a second DISCONNECT,  *)
(*              a redial while M drains).                                 *)
(*   Released -- once the host has asked again, M no longer holds its    *)
(*              DISCONNECT (N3JXB, 1.9.22, 2026-11-04).                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS NS,      \* sessions the caller may place
          SIDS,    \* session ids on the air
          R,       \* DISCONNECT retries (ARQ_DISCONNECT_RETRY_SLOTS)
          QMAX,    \* frames queued each way (bounds TLC)
          LOSS,    \* the channel may lose frames
          FOREIGN, \* another station's DISCONNECT may be heard
          FPAT,    \* ...or another station's pattern
          FEND,    \* ...or another station's M_END
          CALLS,   \* the callsigns the caller may call with (MYCALL per application)
          Mutant   \* "none", or one rule broken:
                   \*   "crossedsilent": ending, the peer's DISCONNECT before mine
                   \*                    went out ends me without a reply
                   \*   "samecall":      ending, only a CALL from the callsign of the
                   \*                    session being torn down ends it (before
                   \*                    the on-air NNCP-after-UUCP failure)
                   \*   "anypattern":    a pattern ends an end whatever its state,
                   \*                    not only one whose DISCONNECT is out
                   \*   "ignorepattern": an end waiting for the answer to its
                   \*                    DISCONNECT does not take the pattern
                   \*   "unkeyed":       a pattern is taken whatever session keyed it
                   \*   "unkeyedend":    an M_END is taken whatever session sent it
                   \*   "endheld":       an M_END is taken while S holds a word M
                   \*                    has not got (status_seen, 2026-10-18)
                   \*   "staleteardown": a CALL taken keeps the DISCONNECT the
                   \*                    last session held back to drain (the
                   \*                    flag that outlived it, 2026-10-19)
                   \*   "skipreply":     an end owing the reply to its peer's
                   \*                    DISCONNECT goes idle without keying it
                   \*                    (another transmission's TX_COMPLETE)
                   \*   "ackless":       S's application hangs up owing the ack
                   \*                    of M's last word, and S's DISCONNECT
                   \*                    takes that ack's slot (before
                   \*                    car_owes_ack, KG9NI-K7EK 2026-11-04)
                   \*   "rearm":         a second DISCONNECT while M drains
                   \*                    re-arms the drain (N3JXB, 2026-11-04)
                   \*   "redialwait":    a CONNECT while M drains waits behind
                   \*                    it, the old session keying on
                   \*   "endsilence":    M ends on silence without having heard
                   \*                    an M_END (MEndSilent unguarded)

E == {"M", "S"}
Other(e) == IF e = "M" THEN "S" ELSE "M"

VARIABLES
    st,      \* [E -> {"idle", "calling", "conn", "ending", "reply"}]
             \*   "reply": ended, its reply to the peer's DISCONNECT still to key
    gen,     \* [E -> 0..NS]: the session it is (or last was) in -- the nonce
    sid,     \* [E -> SIDS \cup {0}]: that session's id on the air
    sent,    \* [E -> BOOLEAN]: its DISCONNECT went out (ARQ_TD_SENT)
    tries,   \* M's DISCONNECT retries left
    slot,    \* S holds a slot (heard an M poll since its last keydown)
    q,       \* [E -> Seq(frame)]: frames on their way to e
    placed,  \* sessions placed so far
    mcall,   \* the callsign M calls with this session; S keeps its caller's in scall
    scall,
    how,     \* ghost: {[e, g, why]} -- how each end left each session
    dropped, \* ghost: S dropped a CALL for it while idle or ending
    wish,    \* S: the session whose application has hung up, its DISCONNECT
             \*   waiting for the drain (ARQ_TD_DRAINING); 0 none
    unasked, \* ghost: M began ending on another session's M_END
    word,    \* S's last word: "none", "held" (written before the hang-up, not
             \*   yet with M) or "got" (M has it)
    lostword, \* ghost: M began ending on an M_END while S held its word
    mw,      \* M's last word this session: "none"; "owed" (S has it, its ack
             \*   not yet keyed: car_owes_ack); "acked" (S's ack is out);
             \*   "conf" (M heard it)
    mdrain,  \* M, connected: its application hung up and the DISCONNECT is held
             \*   to deliver M's last word (ARQ_TD_DRAINING)
    asked,   \* ghost: the host asked again (DISCONNECT, CONNECT) while M held
             \*   its DISCONNECT, and M held it on
    forced,  \* ghost: the host had M drop what its drain had not delivered
    mend,    \* M heard an M_END of this session, acted on or not
             \*   (car_peer_ending: end_req = CAR_END_WISH on M)
    cut,     \* ghost: M ended a session on silence while S, in it, had not
             \*   hung up
    wordgone \* ghost: M ended on silence after the M_END with S's last word
             \*   not yet got (lost to the channel, as at the watchdog)

vars == <<st, gen, sid, sent, tries, slot, q, placed, mcall, scall, how, dropped, wish, unasked, word, lostword, mw,
         mdrain, asked, forced, mend, cut, wordgone>>

Frame(k, s, g) == [k |-> k, sid |-> s, g |-> g, c |-> mcall]
\* The reply to a DISCONNECT is an ACK pattern (send_disconnect_reply): no
\* session id, but keyed by the session's seed, which the replying end keeps
\* until the reply is out (car_stop).  g is that session (0: another
\* station's).
Pattern(g) == [k |-> "pat", sid |-> 0, g |-> g, c |-> mcall]
CallFrame(s, g, c) == [k |-> "call", sid |-> s, g |-> g, c |-> c]
Send(e, f) == Len(q[e]) < QMAX /\ q' = [q EXCEPT ![e] = Append(@, f)]     \* (TLC's bound: waits for room)
Pop(e) == q' = [q EXCEPT ![e] = Tail(@)]
End(e, why) == how' = how \cup {[e |-> e, g |-> gen[e], why |-> why]}

Init == /\ st = [e \in E |-> "idle"] /\ gen = [e \in E |-> 0] /\ sid = [e \in E |-> 0]
        /\ sent = [e \in E |-> FALSE] /\ tries = 0 /\ slot = FALSE
        /\ q = [e \in E |-> <<>>] /\ placed = 0 /\ how = {}
        /\ mcall = (CHOOSE c \in CALLS : TRUE) /\ scall = mcall /\ dropped = FALSE
        /\ wish = 0 /\ unasked = FALSE /\ word = "none" /\ lostword = FALSE /\ mw = "none"
        /\ mdrain = FALSE /\ asked = FALSE /\ forced = FALSE
        /\ mend = FALSE /\ cut = FALSE /\ wordgone = FALSE

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
        /\ \E s \in SIDS, c \in CALLS :
             /\ placed' = placed + 1 /\ mcall' = c
             /\ st' = [st EXCEPT !["M"] = "calling"] /\ gen' = [gen EXCEPT !["M"] = placed + 1]
             /\ sid' = [sid EXCEPT !["M"] = s] /\ sent' = [sent EXCEPT !["M"] = FALSE]
             /\ Send("S", CallFrame(s, placed + 1, c))
        /\ mw' = "none" /\ mdrain' = FALSE /\ asked' = FALSE /\ forced' = FALSE /\ mend' = FALSE
        /\ UNCHANGED <<cut, wordgone>>
        /\ UNCHANGED <<tries, slot, scall, how, dropped, wish, unasked, word, lostword>>
CallFails == /\ st["M"] = "calling" /\ TimerOK("M")
             /\ st' = [st EXCEPT !["M"] = "idle"] /\ End("M", "nocall")
             /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
Poll == /\ st["M"] = "conn"
        /\ Send("S", Frame("poll", sid["M"], gen["M"]))
        /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* The application hangs up with nothing of M's left to deliver: the DISCONNECT
\* goes now.
MAppDisc == /\ st["M"] = "conn" /\ ~mdrain /\ mw \notin {"owed", "acked"}
            /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
            /\ tries' = R
            /\ UNCHANGED <<gen, sid, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* ...with its last word not yet confirmed: the DISCONNECT is held while the
\* drain delivers it (fsm_connected_carousel, ARQ_TD_DRAINING).  The host has
\* been told DISCONNECTED already (arq.c, the VARA convention).
MHold == /\ st["M"] = "conn" /\ ~mdrain /\ mw \in {"owed", "acked"}
         /\ mdrain' = TRUE
         /\ UNCHANGED <<st, gen, sid, sent, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, asked, forced, mend, cut, wordgone>>
\* The drain delivers it: the DISCONNECT goes.
MDrained == /\ st["M"] = "conn" /\ mdrain /\ mw \notin {"owed", "acked"}
            /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
            /\ tries' = R /\ mdrain' = FALSE
            /\ UNCHANGED <<gen, sid, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, asked, forced, mend, cut, wordgone>>
\* Its deadline passes, or the peer has been silent ARQ_CAR_PEER_QUIET_MS: the
\* DISCONNECT goes whatever is undelivered (a timer: only once nothing more
\* is coming).
MDrainEnd == /\ st["M"] = "conn" /\ mdrain /\ TimerOK("M")
             /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
             /\ tries' = R /\ mdrain' = FALSE
             /\ UNCHANGED <<gen, sid, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, asked, forced, mend, cut, wordgone>>
\* The host, told DISCONNECTED, asks again while the drain holds: a second
\* DISCONNECT, or a CONNECT (the redial, placed once M is idle: Call).  Each
\* ends the drain at once and drops what it has not delivered, as ABORT
\* does.  Before (N3JXB, 1.9.22, 2026-11-04) a second DISCONNECT re-armed the
\* drain and a CONNECT waited behind it, the old session keying on.
HostAgain(mut) == /\ st["M"] = "conn" /\ mdrain
                  /\ IF Mutant = mut
                     THEN /\ asked' = TRUE
                          /\ UNCHANGED <<st, sent, tries, mdrain, forced>>
                     ELSE /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
                          /\ tries' = R /\ mdrain' = FALSE /\ forced' = TRUE
                          /\ UNCHANGED asked
                  /\ UNCHANGED <<gen, sid, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mend, cut, wordgone>>
MAgain  == HostAgain("rearm")
MRedial == placed < NS /\ HostAgain("redialwait")
\* S said its application hung up (M_END) and has been silent since, as long
\* as its own drain would last (a timer: only once nothing more is coming):
\* M ends the session with one DISCONNECT, no retries, whatever of S's last
\* word has not come (fsm_connected_carousel, car_peer_end_wait_ms; N3JXB ->
\* KG9NI-1, 1.9.22: the watchdog's 240 s before, and no DISCONNECT).
MEndSilent == /\ st["M"] = "conn" /\ (mend \/ Mutant = "endsilence") /\ TimerOK("M")
              /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
              /\ tries' = 0 /\ mdrain' = FALSE
              /\ cut' = (cut \/ (gen["S"] = gen["M"] /\ st["S"] \in {"conn", "ending", "reply"} /\ wish # gen["M"]))
              /\ wordgone' = (wordgone \/ (word = "held" /\ gen["S"] = gen["M"]))
              /\ UNCHANGED <<gen, sid, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw,
                             asked, forced, mend>>
MDisc == /\ st["M"] = "ending" /\ ~sent["M"]
         /\ sent' = [sent EXCEPT !["M"] = TRUE]
         /\ Send("S", Frame("disc", sid["M"], gen["M"]))
         /\ UNCHANGED <<st, gen, sid, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
MRetry == /\ st["M"] = "ending" /\ sent["M"] /\ tries > 0 /\ TimerOK("M")
          /\ tries' = tries - 1
          /\ Send("S", Frame("disc", sid["M"], gen["M"]))
          /\ UNCHANGED <<st, gen, sid, sent, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
MGiveUp == /\ st["M"] = "ending" /\ sent["M"] /\ tries = 0 /\ TimerOK("M")
           /\ st' = [st EXCEPT !["M"] = "idle"] /\ End("M", "timeout")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>

\* M's last word reaches S's application (rounds and their repair,
\* abstracted): S now owes an answer acknowledging it.
MWord == /\ mw = "none" /\ st["M"] = "conn" /\ ~mdrain /\ st["S"] = "conn" /\ gen["M"] = gen["S"] /\ wish = 0
         /\ mw' = "owed"
         /\ UNCHANGED <<st, gen, sid, sent, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mdrain, asked, forced, mend, cut, wordgone>>

\* ---- the callee ----
\* The application hangs up with nothing owed either way: the DISCONNECT goes
\* in the next slot.
SAppDisc == /\ st["S"] = "conn" /\ (mw # "owed" \/ Mutant = "ackless")
            /\ st' = [st EXCEPT !["S"] = "ending"] /\ sent' = [sent EXCEPT !["S"] = FALSE]
            /\ UNCHANGED <<gen, sid, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* The application hangs up with data of S's not yet acknowledged, or owing
\* the ack of M's last word (car_owes_ack): the DISCONNECT waits
\* (ARQ_TD_DRAINING, car_wish_end), and S's answers say so; the drain then
\* completes (or its deadline passes).
SWish == /\ st["S"] = "conn" /\ wish = 0
         /\ wish' = gen["S"] /\ word' \in {"none", "held"}
         /\ UNCHANGED <<st, gen, sid, sent, tries, slot, q, placed, how, mcall, scall, dropped, unasked, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* S's last word reaches M: rounds and their repair, abstracted (any loss
\* only delays it while both are in the session).
SDeliver == /\ word = "held" /\ st["S"] = "conn" /\ st["M"] = "conn" /\ gen["M"] = gen["S"]
            /\ word' = "got"
            /\ UNCHANGED <<st, gen, sid, sent, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* S's answer in its slot acknowledges what it has of M's: an M_END while
\* its application is hanging up, an ordinary ack otherwise.
SAck == /\ st["S"] = "conn" /\ wish = 0 /\ mw = "owed" /\ slot
        /\ slot' = FALSE /\ mw' = "acked"
        /\ Send("M", Frame("ack", sid["S"], gen["S"]))
        /\ UNCHANGED <<st, gen, sid, sent, tries, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mdrain, asked, forced, mend, cut, wordgone>>
SEndPoll == /\ st["S"] = "conn" /\ wish # 0 /\ slot
            /\ slot' = FALSE /\ mw' = (IF mw = "owed" THEN "acked" ELSE mw)
            /\ Send("M", Frame("end", sid["S"], gen["S"]))
            /\ UNCHANGED <<st, gen, sid, sent, tries, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mdrain, asked, forced, mend, cut, wordgone>>
\* ...and once nothing is owed either way, its DISCONNECT goes in its next slot.
SDrained == /\ st["S"] = "conn" /\ wish # 0 /\ word # "held" /\ mw # "owed"
            /\ st' = [st EXCEPT !["S"] = "ending"] /\ sent' = [sent EXCEPT !["S"] = FALSE]
            /\ UNCHANGED <<gen, sid, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
SSlotDisc == /\ st["S"] = "ending" /\ slot
             /\ slot' = FALSE /\ sent' = [sent EXCEPT !["S"] = TRUE]
             /\ Send("M", Frame("disc", sid["S"], gen["S"]))
             /\ UNCHANGED <<st, gen, sid, tries, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
SGiveUp == /\ st["S"] = "ending" /\ TimerOK("S")
           /\ st' = [st EXCEPT !["S"] = "idle"] /\ End("S", "timeout")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>

\* ---- either end ----
Lost(e) == /\ st[e] = "conn" /\ TimerOK(e)
           /\ st' = [st EXCEPT ![e] = "idle"] /\ End(e, "lost")
           /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
Reply(e) == /\ st[e] = "reply"
            /\ st' = [st EXCEPT ![e] = "idle"]
            /\ Send(Other(e), Pattern(gen[e]))
            /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* The end of another transmission while the reply is owed (TX_COMPLETE):
\* only the reply's own ends the teardown (ARQ_TD_REPLYING), so only under
\* the mutant.
SkipReply(e) == /\ Mutant = "skipreply" /\ st[e] = "reply"
                /\ st' = [st EXCEPT ![e] = "idle"]
                /\ UNCHANGED <<gen, sid, sent, tries, slot, q, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>

\* S takes a CALL when idle; ending (fsm_disconnecting: RX_CALL), from its
\* caller's callsign -- or from any once its DISCONNECT is out and it only
\* waits for the answer.
Waiting == st["S"] = "ending" /\ sent["S"]
Takes(f) == \/ st["S"] = "idle"
            \/ st["S"] = "ending" /\ f.c = scall
            \/ Waiting /\ Mutant # "samecall"
\* e takes the frame at the head of its queue.
Mine(e, f) == f.g = gen[e]                                   \* seeded: its session's
HearDisc(e, f) ==
    /\ f.k = "disc" /\ st[e] \in {"conn", "ending"}
    /\ f.sid = sid[e]
    /\ LET why == IF f.g = gen[e] THEN "peer" ELSE "killed" IN
       IF st[e] = "ending" /\ (sent[e] \/ Mutant = "crossedsilent")
       THEN st' = [st EXCEPT ![e] = "idle"] /\ End(e, why)
       ELSE st' = [st EXCEPT ![e] = "reply"] /\ End(e, why)
    /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* A pattern ends only an end whose DISCONNECT is out and that waits for the
\* answer (fsm_disconnecting: RX_PATTERN; the caller's, or the callee's in
\* its ending carousel); anywhere else it is the carousel's, not modelled.
TakesPat(e) == CASE Mutant = "anypattern"    -> st[e] \in {"conn", "ending"}
                  [] Mutant = "ignorepattern" -> FALSE
                  [] OTHER                    -> st[e] = "ending" /\ sent[e]
HearPat(e, f) ==
    /\ f.k = "pat" /\ TakesPat(e) /\ (Mine(e, f) \/ Mutant = "unkeyed")
    /\ st' = [st EXCEPT ![e] = "idle"]
    /\ End(e, IF f.g = gen[e] THEN "peer" ELSE IF f.g = 0 THEN "foreign" ELSE "stale")
    /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
\* An M_END is a carousel control frame: seeded, so only its session's.
\* M takes it holding all S has: the poll names S's latest block and whether
\* bytes wait unopened, and M knows which it holds (peer_ended).
TakesEnd(f) == /\ f.k = "end" /\ st["M"] = "conn" /\ (Mine("M", f) \/ Mutant = "unkeyedend")
               /\ (word # "held" \/ f.g # gen["S"] \/ Mutant = "endheld")
Receive(e) ==
    /\ q[e] # <<>>
    /\ LET f == Head(q[e]) IN
       \/ HearDisc(e, f) /\ Pop(e)
       \/ HearPat(e, f) /\ Pop(e)
       \* S answers a CALL when idle, or when ending and its peer calls again
       \/ /\ e = "S" /\ f.k = "call" /\ Takes(f)
          /\ st' = [st EXCEPT !["S"] = "conn"] /\ gen' = [gen EXCEPT !["S"] = f.g]
          /\ scall' = f.c /\ wish' = (IF Mutant = "staleteardown" THEN wish ELSE 0) /\ word' = "none" /\ UNCHANGED <<dropped, unasked, lostword, mw>>
          /\ sid' = [sid EXCEPT !["S"] = f.sid] /\ sent' = [sent EXCEPT !["S"] = FALSE]
          /\ slot' = FALSE
          /\ IF st["S"] = "ending" THEN End("S", "recall") ELSE UNCHANGED how
          /\ Len(q["M"]) < QMAX
          /\ q' = [q EXCEPT !["S"] = Tail(@), !["M"] = Append(@, Frame("accept", f.sid, f.g))]
          /\ UNCHANGED <<tries, placed, mcall, mdrain, asked, forced, mend, cut, wordgone>>
       \/ /\ e = "M" /\ f.k = "accept" /\ st["M"] = "calling" /\ Mine("M", f)
          /\ st' = [st EXCEPT !["M"] = "conn"] /\ Pop("M")
          /\ UNCHANGED <<gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
       \* M, connected, hears S's M_END: it ends the session as for its application
       \* (the M_END acknowledges what S has of M's)
       \/ /\ e = "M" /\ TakesEnd(f)
          /\ st' = [st EXCEPT !["M"] = "ending"] /\ sent' = [sent EXCEPT !["M"] = FALSE]
          /\ tries' = R /\ unasked' = (unasked \/ f.g # gen["M"]) /\ Pop("M")
          /\ lostword' = (lostword \/ (word = "held" /\ gen["S"] = gen["M"]))
          /\ mw' = (IF Mine("M", f) /\ mw = "acked" THEN "conf" ELSE mw)
          /\ UNCHANGED <<gen, sid, slot, placed, how, mcall, scall, dropped, wish, word, mdrain, asked, forced, mend, cut, wordgone>>
       \* M, connected, hears S's ack of its session
       \/ /\ e = "M" /\ f.k = "ack" /\ st["M"] = "conn" /\ Mine("M", f)
          /\ mw' = (IF mw = "acked" THEN "conf" ELSE mw) /\ Pop("M")
          /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mdrain, asked, forced, mend, cut, wordgone>>
       \/ /\ e = "S" /\ f.k = "poll" /\ st["S"] \in {"conn", "ending"} /\ Mine("S", f)
          /\ slot' = TRUE /\ Pop("S")
          /\ UNCHANGED <<st, gen, sid, sent, tries, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
       \* anything else: not for it, or not in its state -- dropped
       \/ /\ ~(f.k = "disc" /\ st[e] \in {"conn", "ending"} /\ f.sid = sid[e])
          /\ ~(f.k = "pat" /\ TakesPat(e) /\ (Mine(e, f) \/ Mutant = "unkeyed"))
          /\ ~(e = "S" /\ f.k = "call" /\ Takes(f))
          /\ ~(e = "M" /\ f.k = "accept" /\ st["M"] = "calling" /\ Mine("M", f))
          /\ ~(e = "S" /\ f.k = "poll" /\ st["S"] \in {"conn", "ending"} /\ Mine("S", f))
          /\ ~(e = "M" /\ TakesEnd(f))
          /\ ~(e = "M" /\ f.k = "ack" /\ st["M"] = "conn" /\ Mine("M", f))
          /\ Pop(e)
          /\ dropped' = (dropped \/ (e = "S" /\ f.k = "call" /\ (st["S"] = "idle" \/ Waiting)))
          \* an M_END M does not act on still acknowledges what S has of M's
          /\ mw' = (IF e = "M" /\ f.k = "end" /\ st["M"] = "conn" /\ Mine("M", f) /\ mw = "acked"
                    THEN "conf" ELSE mw)
          \* ...and M remembers it (car_peer_ending, end_req on M)
          /\ mend' = (mend \/ (e = "M" /\ f.k = "end" /\ st["M"] = "conn" /\ Mine("M", f)))
          /\ UNCHANGED <<wish, unasked, word, lostword>>
          /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, mcall, scall, how, mdrain, asked, forced, cut, wordgone>>

\* ---- the channel ----
Lose(e) == LOSS /\ q[e] # <<>> /\ Pop(e) /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>
Foreign(e) == /\ \/ FOREIGN /\ \E s \in SIDS : Send(e, Frame("disc", s, 0))
                 \/ FEND /\ e = "M" /\ \E s \in SIDS : Send(e, Frame("end", s, 0))
                 \/ FPAT /\ Send(e, Pattern(0))
              /\ UNCHANGED <<st, gen, sid, sent, tries, slot, placed, how, mcall, scall, dropped, wish, unasked, word, lostword, mw, mdrain, asked, forced, mend, cut, wordgone>>

\* All sessions placed and over: TLC stops here.
Done == placed = NS /\ \A e \in E : st[e] = "idle" /\ q[e] = <<>>
        /\ UNCHANGED vars

Next == \/ Done \/ Call \/ CallFails \/ Poll \/ MAppDisc \/ MDisc \/ MRetry \/ MGiveUp
        \/ MHold \/ MDrained \/ MDrainEnd \/ MAgain \/ MRedial \/ MEndSilent
        \/ MWord \/ SAppDisc \/ SWish \/ SDeliver \/ SAck \/ SEndPoll \/ SDrained \/ SSlotDisc \/ SGiveUp
        \/ \E e \in E : Lost(e) \/ Reply(e) \/ SkipReply(e) \/ Receive(e) \/ Lose(e) \/ Foreign(e)

\* Progress is fair; the application, the channel's losses and other
\* stations are not.
Fair == /\ WF_vars(MDisc) /\ WF_vars(MRetry) /\ WF_vars(MGiveUp) /\ WF_vars(CallFails)
        /\ WF_vars(MDrained) /\ WF_vars(MDrainEnd)
        /\ WF_vars(SSlotDisc) /\ WF_vars(SGiveUp) /\ WF_vars(Poll)
        /\ WF_vars(SEndPoll) /\ WF_vars(SDrained) /\ WF_vars(SDeliver) /\ WF_vars(SAck)
        /\ \A e \in E : WF_vars(Reply(e)) /\ WF_vars(Receive(e)) /\ WF_vars(Lost(e))
Spec == Init /\ [][Next]_vars /\ Fair

\* ---- properties ----
TypeOK == /\ st \in [E -> {"idle", "calling", "conn", "ending", "reply"}]
          /\ tries \in 0 .. R /\ placed \in 0 .. NS /\ wish \in 0 .. NS
          /\ mw \in {"none", "owed", "acked", "conf"}
          /\ mdrain \in BOOLEAN /\ asked \in BOOLEAN /\ forced \in BOOLEAN
          /\ mend \in BOOLEAN /\ cut \in BOOLEAN /\ wordgone \in BOOLEAN
NoKill == \A h \in how : h.why # "killed"
\* No end leaves on the answer to an earlier session's DISCONNECT, nor on
\* another station's pattern.
NoStale == \A h \in how : h.why # "stale"
NoForeign == \A h \in how : h.why # "foreign"
Answered == ~dropped
NoUnasked == ~unasked
\* M ending on an M_END while S held its word.  Not M ending on silence after
\* one (MEndSilent): that word is lost to the channel, as it was at the
\* lost-peer watchdog, and the ghost wordgone records it instead.
NoLostWord == ~lostword
\* M ends a session on silence (MEndSilent) only once S's application has
\* hung up in it: a fade in a live session is left to the watchdog.
NoCut == ~cut
NoInherited == wish # 0 => wish = gen["S"]
Clean == ~LOSS => \A h \in how : h.why \in {"peer", "recall"}
\* M out of the session (or replying to S's DISCONNECT) with its last word
\* of the session unacknowledged.  An ack may be lost: lossless only.
\* Not when the host had M drop it (a second DISCONNECT, a redial: forced).
Confirmed == ~LOSS /\ ~forced => ~(st["M"] \in {"idle", "reply"} /\ mw \in {"owed", "acked"})
\* Once the host has asked again, M holds its DISCONNECT no longer.
Released == ~(st["M"] = "conn" /\ mdrain /\ asked)
In(e, g) == gen[e] = g /\ st[e] \in {"calling", "conn", "ending", "reply"}
Mutual == \A g \in 1 .. NS :
            (\E h \in how : h.g = g /\ h.why # "nocall") ~> ~(\E e \in E : In(e, g))
=============================================================================
