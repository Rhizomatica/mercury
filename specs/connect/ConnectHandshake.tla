--------------------------- MODULE ConnectHandshake ----------------------------
(***************************************************************************)
(* The connect handshake, CALL / ACCEPT / first carousel frame, as         *)
(* datalink_arq/arq_fsm.c runs it (fsm_disconnected, fsm_listening,        *)
(* fsm_calling, fsm_accepting, car_connect_callee):                        *)
(*                                                                         *)
(*   C, the caller, on CONNECT picks a session id and CALLs, R times at    *)
(*   most, then gives up.  It takes an ACCEPT only for its session (id     *)
(*   and both callsign CRCs) and then is connected: it holds the ACCEPT's  *)
(*   nonce (in the session seed, car_seed) and start rung (and with it the *)
(*   piece size, car_piece_for_start), and sends its first round there.   *)
(*                                                                         *)
(*   E, the callee, hearing a CALL while listening, picks a nonce and      *)
(*   measures the start rung, and ACCEPTs.  Every repeat of that session's *)
(*   CALL gets the same ACCEPT (spec R25); another session's CALL is       *)
(*   ignored.  Out of ACCEPT retries it listens again, keeping the seed    *)
(*   (accept_fallback): a later CALL of the same session gets the same     *)
(*   ACCEPT, another session's a fresh one.  It is connected when it       *)
(*   decodes C's first frame: on the rung it named, and only with the     *)
(*   nonce it named (the frame's CRC is seeded).                          *)
(*                                                                         *)
(*   C, out of CALLs, may hear an ACCEPT of its session: it sends a       *)
(*   DISCONNECT, and E listens again.  Connected, C gives up on an E that *)
(*   never answers after ARQ_CAR_PEER_LOST_MS, when E's kept seed, kept   *)
(*   as long, also lapses (one step here: PeerLost).                       *)
(*                                                                         *)
(* An exchange (a keydown and the answer to it) is one step; the turn     *)
(* rules keep keydowns apart (specs/turns).  Any frame may be lost.  Not  *)
(* modelled: the carrier (DATAC16 or MFSK: a CALL or ACCEPT is lost or   *)
(* not), other stations (their CALLs are ignored like another session's), *)
(* the disconnect of a session once up.                                   *)
(*                                                                         *)
(* Properties:                                                            *)
(*   NoDoom         C never holds an ACCEPT E no longer stands by (its   *)
(*                  frames would fail E's CRC or land on the wrong rung, *)
(*                  or the two would cap rungs and pick floors apart);   *)
(*   NoHalfConnect  E connected means C connected, same session, nonce;   *)
(*   Settles        it ends with both connected, or both idle.            *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS Sids,     \* session ids C may pick
          Nonces,   \* E's nonces
          Levels,   \* start rungs E may measure
          R,        \* C's CALLs per CONNECT
          S,        \* CONNECTs the application issues
          Bws,      \* bandwidths a CALL may advertise
          Mutant    \* "none", or one rule broken (to show the model can fail):
                    \*   "renonce":   a repeated CALL gets a fresh nonce
                    \*   "remeasure": a repeated CALL gets a re-measured start rung
                    \*   "bwleak":    another station's CALL sets the session
                    \*                bandwidth (arq.c before the FSM owned it)

VARIABLES
    cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw,
    eSt, eSid, eNonce, eLevel, eBw,
    sent      \* every ACCEPT E has keyed: C may decode any of them, late
              \* (one in flight while its next CALL already went out: R25)

vars == <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent>>
cVars == <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw>>

Init ==
    /\ cSt = "idle" /\ cSid = 0 /\ cCalls = 0 /\ cNonce = 0 /\ cLevel = 0 /\ cSessions = 0 /\ cBw = 0
    /\ eSt = "listening" /\ eSid = 0 /\ eNonce = 0 /\ eLevel = 0 /\ eBw = 0
    /\ sent = {}

\* E holds a session's seed: answering its CALLs, or keeping it after them.
EHolds == eSt \in {"accepting", "fallback"}

\* The application asks C to connect: a new session.
AppConnect ==
    /\ cSt = "idle" /\ cSessions < S
    /\ \E s \in Sids, b \in Bws : cSid' = s /\ cBw' = b
    /\ cSt' = "calling" /\ cCalls' = R /\ cSessions' = cSessions + 1
    /\ cNonce' = 0 /\ cLevel' = 0
    /\ UNCHANGED <<eSt, eSid, eNonce, eLevel, eBw, sent>>

\* E's answer to a CALL of session s: the ACCEPT it would send, and its state.
\* Fresh: a new nonce and a new measurement.  The same session: the same
\* ACCEPT (unless a mutant re-picks).
EAnswer(s, nonce, level) ==
    /\ eSt' = "accepting" /\ eSid' = s
    /\ IF EHolds /\ eSid = s
       THEN /\ eNonce' = (IF Mutant = "renonce" THEN nonce ELSE eNonce)
            /\ eLevel' = (IF Mutant = "remeasure" THEN level ELSE eLevel)
            /\ eBw' = eBw            \* a repeat keeps the session's bandwidth
       ELSE /\ eNonce' = nonce /\ eLevel' = level
            /\ eBw' = cBw            \* the CALL's (min with E's own: folded into Bws)

\* C keys a CALL; E may hear it and ACCEPT.
Call ==
    /\ cSt = "calling" /\ cCalls > 0
    /\ cCalls' = cCalls - 1
    /\ UNCHANGED <<cSt, cSid, cNonce, cLevel, cSessions, cBw>>
    /\ \E heard \in BOOLEAN, nonce \in Nonces, level \in Levels :
        IF ~heard \/ eSt = "connected" \/ (eSt = "accepting" /\ eSid # cSid)
        THEN \* lost, or E is busy with another session: no ACCEPT
             UNCHANGED <<eSt, eSid, eNonce, eLevel, eBw, sent>>
        ELSE /\ EAnswer(cSid, nonce, level)
             \* ACCEPTs of E's current session only: an older one's left the air
             /\ sent' = (IF EHolds /\ eSid = cSid THEN sent ELSE {})
                        \cup {[sid |-> cSid, nonce |-> eNonce', level |-> eLevel', bw |-> eBw']}

\* C, still calling, decodes an ACCEPT of its session: any E keyed.
HearAccept ==
    /\ cSt = "calling"
    /\ \E a \in sent :
        /\ a.sid = cSid
        /\ cSt' = "connected" /\ cNonce' = a.nonce /\ cLevel' = a.level /\ cBw' = a.bw
    /\ UNCHANGED <<cSid, cCalls, cSessions, eSt, eSid, eNonce, eLevel, eBw, sent>>

\* Out of CALLs: C gives up.
CallGiveUp ==
    /\ cSt = "calling" /\ cCalls = 0
    /\ cSt' = "idle"
    /\ UNCHANGED <<cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent>>

\* Out of ACCEPT retries: E listens again, keeping the seed.
EFallback ==
    /\ eSt = "accepting"
    /\ eSt' = "fallback"
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw, sent>>

\* The kept seed lapses (ARQ_CAR_PEER_LOST_MS) -- with no caller connected on
\* it; one that is gives up at the same time (PeerLost).
EExpire ==
    /\ eSt = "fallback" /\ ~(cSt = "connected" /\ cSid = eSid)
    /\ eSt' = "listening"
    /\ sent' = {} /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw>>

\* An ACCEPT of C's session reaches a C that gave up: it sends a DISCONNECT.
StaleAccept ==
    /\ cSt = "idle" /\ eSt = "accepting" /\ eSid = cSid
    /\ eSt' = "listening"
    /\ sent' = {} /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw>>

\* Another station's CALL for E while E answers C's: ignored -- but with the
\* bandwidth leak, it set the session bandwidth E's next ACCEPT says.
ForeignCall ==
    /\ eSt = "accepting"
    /\ \E b \in Bws : eBw' = (IF Mutant = "bwleak" THEN b ELSE eBw)
    /\ sent' = {[a EXCEPT !.bw = eBw'] : a \in sent}
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel>>

\* C's first round reaches E: on the rung E named, with E's nonce in its CRC.
\* (On another bandwidth the ends cap their rungs and pick their floor apart:
\* NoDoom catches it.)
FirstFrame ==
    /\ cSt = "connected" /\ EHolds /\ eSid = cSid
    /\ cNonce = eNonce /\ cLevel = eLevel
    /\ eSt' = "connected"
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw, sent>>

\* C connected, E never: both give up (ARQ_CAR_PEER_LOST_MS each).
PeerLost ==
    /\ cSt = "connected" /\ eSt # "connected"
    /\ cSt' = "idle"
    /\ eSt' = (IF eSt = "fallback" /\ eSid = cSid THEN "listening" ELSE eSt)
    /\ sent' = (IF eSt' = "listening" THEN {} ELSE sent)
    /\ UNCHANGED <<cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw>>

\* Over: both connected, or both idle with the application done connecting.
Done == /\ \/ cSt = "connected" /\ eSt = "connected"
           \/ cSt = "idle" /\ cSessions = S /\ ~EHolds
        /\ UNCHANGED vars

Next == AppConnect \/ Call \/ HearAccept \/ CallGiveUp \/ EFallback \/ EExpire \/ StaleAccept
        \/ ForeignCall \/ FirstFrame \/ PeerLost \/ Done

\* Timers run out and the channel is not an adversary for ever: C's first
\* round, possible again and again, gets through at last.
Spec == Init /\ [][Next]_vars
        /\ WF_vars(Call) /\ WF_vars(CallGiveUp) /\ WF_vars(EExpire) /\ WF_vars(PeerLost) /\ WF_vars(EFallback)
        /\ SF_vars(FirstFrame)

\* ---- properties ----
TypeOK ==
    /\ cSt \in {"idle", "calling", "connected"} /\ cSid \in Sids \cup {0}
    /\ cBw \in Bws \cup {0} /\ eBw \in Bws \cup {0}
    /\ cCalls \in 0 .. R /\ cSessions \in 0 .. S
    /\ cNonce \in Nonces \cup {0} /\ cLevel \in Levels \cup {0}
    /\ eSt \in {"listening", "accepting", "fallback", "connected"} /\ eSid \in Sids \cup {0}
    /\ eNonce \in Nonces \cup {0} /\ eLevel \in Levels \cup {0}

NoDoom == (cSt = "connected" /\ (EHolds \/ eSt = "connected") /\ eSid = cSid) =>
              (cNonce = eNonce /\ cLevel = eLevel /\ cBw = eBw)
NoHalfConnect == eSt = "connected" => (cSt = "connected" /\ cSid = eSid /\ cNonce = eNonce /\ cLevel = eLevel)
Settles == <>[]((cSt = "connected" /\ eSt = "connected") \/ (cSt = "idle" /\ ~EHolds))
=============================================================================
