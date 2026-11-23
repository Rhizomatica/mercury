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
(*   CALL gets the same ACCEPT (spec R25).  C's CALL of another session   *)
(*   (it CONNECTed again: an id per CONNECT) means C has left the one E   *)
(*   answers: E answers the new one, fresh (fsm_accepting) -- except      *)
(*   while E's ACCEPT of the one it answers could still be answered, and  *)
(*   never one decoded too late: a stale frame is not a redial.  Out of   *)
(*   ACCEPT retries it listens again, keeping the seed                    *)
(*   (accept_fallback): a later CALL of the same session gets the same     *)
(*   ACCEPT, another session's a fresh one.  It is connected when it       *)
(*   decodes C's first frame: on the rung it named, and only with the     *)
(*   nonce it named (the frame's CRC is seeded).                          *)
(*                                                                         *)
(*   C, out of CALLs, may hear an ACCEPT of its session: it sends a       *)
(*   DISCONNECT, and E listens again.  Connected, C gives up on an E that *)
(*   never answers after ARQ_CAR_PEER_LOST_MS (PeerLost).  E's kept seed  *)
(*   lapses as long after its last ACCEPT, so later (EExpire): C may      *)
(*   CONNECT again meanwhile, and E answer from its fallback.             *)
(*                                                                         *)
(*   Both connected, C's lost-peer watchdog may fire first (it last heard *)
(*   E earlier than E heard it): C is idle, E still connected until its   *)
(*   own fires.  E connected, hearing C CALL another session, ends its    *)
(*   own -- C has left it -- and answers (fsm_connected, RX_CALL).        *)
(*                                                                         *)
(*   A CALL C keyed for a session it has left may surface at E late       *)
(*   (a decoder backlog; on air, KG9NI <- K7EK, 2026-11-17 21:19).  E,   *)
(*   answering, ignores it (StaleCall): too old, and C is not calling     *)
(*   that session.                                                         *)
(*                                                                         *)
(* An exchange (a keydown and the answer to it) is one step; the turn     *)
(* rules keep keydowns apart (specs/turns).  Any frame may be lost.  Not  *)
(* modelled: the carrier (DATAC16 or MFSK: a CALL or ACCEPT is lost or   *)
(* not; ConnectCarrier models it), other stations (their CALLs are        *)
(* ignored like another session's), the disconnect of a session once up.  *)
(*                                                                         *)
(* Encryption (arq_crypto.h, crypto_negotiate): each CONNECT's CALLs     *)
(* offer it or not (a key for E, the client's ENCRYPT ON/OFF), in the     *)
(* session id's low bit (Offers: odd ids offer); E, taking                *)
(* a CALL, answers the offer once for the session (it holds a key for C,  *)
(* its client has not said OFF) and every ACCEPT of the session carries   *)
(* that answer; C takes an ACCEPT's bit only with an offer of its own.    *)
(* A required end never goes clear: C does not call without offering, E  *)
(* does not answer a CALL it does not take, and C hangs up on an ACCEPT   *)
(* that did not take its offer.                                            *)
(*                                                                         *)
(* Properties:                                                            *)
(*   NoDoom         C never holds an ACCEPT E no longer stands by (its   *)
(*                  frames would fail E's CRC or land on the wrong rung, *)
(*                  or the two would cap rungs and pick floors apart);   *)
(*   NoHalfConnect  E connected means C connected, same session, nonce   *)
(*                  -- or C left that session (its watchdog);             *)
(*   NoDeafRedial   no CALL of C's heard goes unanswered for a session C *)
(*                  has left (E connected in it, or answering it);       *)
(*   NoDeadAnswer   E never answers a session other than the one its    *)
(*                  connected caller is in (a stale CALL's: answering   *)
(*                  it drops the live session's frames);                 *)
(*   Settles        it ends with both connected, or both idle;           *)
(*   Agree          E connected: both ends took the session to be         *)
(*                  encrypted, or both clear (else one waits for a        *)
(*                  handshake the other never sends, and the session      *)
(*                  fails closed);                                        *)
(*   ReqGate        a required end connected is encrypted;                *)
(*   SameLadder     C connected: both ends map the ladder's levels to the *)
(*                  same modes (minors 0 and 1 share a ladder; 2 and 3    *)
(*                  each have their own).  The wire carries a rung's      *)
(*                  level, not its mode.                                  *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS Sids,     \* session ids C may pick
          Nonces,   \* E's nonces
          Levels,   \* start rungs E may measure
          R,        \* C's CALLs per CONNECT
          S,        \* CONNECTs the application issues
          Bws,      \* bandwidths a CALL may advertise
          CReq,     \* C's [crypto] mode is required
          EReq,     \* E's
          CMinor,   \* C's protocol minor (1.9.24: 1, 1.9.25: 2, PSK3: 3)
          EMinor,   \* E's
          CMin,     \* the lowest minor C takes a frame of
                    \* (ARQ_CONNECT_MINOR_MIN; 1.9.20-1.9.24: 0)
          EMin,     \* E's
          Mutant    \* "none", or one rule broken (to show the model can fail):
                    \*   "renonce":   a repeated CALL gets a fresh nonce
                    \*   "remeasure": a repeated CALL gets a re-measured start rung
                    \*   "bwleak":    another station's CALL sets the session
                    \*                bandwidth (arq.c before the FSM owned it)
                    \*   "ignorecall": E connected ignores every CALL (the code
                    \*                before 2026-10-18): a redial waits out E's
                    \*                watchdog
                    \*   "reanswer": a repeated CALL gets a fresh answer to
                    \*                its offer (ACCEPTs of either answer in
                    \*                the air)
                    \*   "anyaccept": C takes an ACCEPT that took an offer this
                    \*                CONNECT did not make (one of an earlier
                    \*                CONNECT of the same id)
                    \*   "ignorenewer": E answering ignores C's CALL of another
                    \*                session (the code before 2026-11-19): C's
                    \*                redial waits out E's ACCEPTs
                    \*   "staletake": E answering answers a stale CALL, one of
                    \*                a session C keyed earlier and left (the
                    \*                switch before the age check, 2026-11-20)
                    \*   "mixed":    a station takes a CALL or ACCEPT of any
                    \*                minor (no arq_protocol_connect_compatible)

VARIABLES
    cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw,
    eSt, eSid, eNonce, eLevel, eBw,
    sent,     \* every ACCEPT E has keyed: C may decode any of them, late
              \* (one in flight while its next CALL already went out: R25)
    eLeft,    \* ghost: E is connected in a session C has left
    deaf,     \* ghost: a CALL of C's heard by such an E went unanswered
    cPast,    \* session ids C has keyed CALLs for and left
    cOffer,   \* this CONNECT's CALLs offer encryption
    cEnc,     \* C connected: the session is encrypted (offer and its ACCEPT's bit)
    eOffer,   \* the offer of the CALL E took for its session
    eTake     \* E's answer to it, in every ACCEPT of the session

vars == <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent, eLeft, deaf,
          cPast, cOffer, cEnc, eOffer, eTake>>
cryptoVars == <<cOffer, cEnc, eOffer, eTake>>
cVars == <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw>>

Init ==
    /\ cSt = "idle" /\ cSid = 0 /\ cCalls = 0 /\ cNonce = 0 /\ cLevel = 0 /\ cSessions = 0 /\ cBw = 0
    /\ eSt = "listening" /\ eSid = 0 /\ eNonce = 0 /\ eLevel = 0 /\ eBw = 0
    /\ sent = {} /\ eLeft = FALSE /\ deaf = FALSE
    /\ cPast = {}
    /\ cOffer = FALSE /\ cEnc = FALSE /\ eOffer = FALSE /\ eTake = FALSE

\* Does a station whose lowest minor is `min` take a CALL or ACCEPT of minor
\* `peer`?  Only from `min` up (arq_protocol_connect_compatible); 1.9.20-1.9.24
\* read any minor (min 0).
Takes(min, peer) == peer >= min \/ Mutant = "mixed"
\* The ladder a minor maps levels with: minors 0 and 1 share one, minor 2
\* (the per-bandwidth ladders) and minor 3 (PSK3 at rung 2) have their own.
Ladder(m) == IF m < 2 THEN 1 ELSE m

\* The offer is the session id's low bit (ARQ_CALL_CRYPTO_OFFER): an ACCEPT,
\* echoing the id, answers that id's offer.
Offers(s) == s % 2 = 1

\* E holds a session's seed: answering its CALLs, or keeping it after them.
EHolds == eSt \in {"accepting", "fallback"}

\* The application asks C to connect: a new session.
AppConnect ==
    /\ cSt = "idle" /\ cSessions < S
    /\ \E s \in Sids, b \in Bws : cSid' = s /\ cBw' = b
    /\ cSt' = "calling" /\ cCalls' = R /\ cSessions' = cSessions + 1
    /\ cNonce' = 0 /\ cLevel' = 0
    \* A required C calls only to offer (arq_crypto_may_call).
    /\ cOffer' = Offers(cSid') /\ (CReq => cOffer')
    /\ cEnc' = FALSE
    /\ UNCHANGED <<eSt, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf, cPast, eOffer, eTake>>

\* E's answer to a CALL of session s: the ACCEPT it would send, and its state.
\* Fresh: a new nonce and a new measurement.  The same session: the same
\* ACCEPT (unless a mutant re-picks).
EAnswer(s, nonce, level, take) ==
    /\ eSt' = "accepting" /\ eSid' = s
    /\ IF EHolds /\ eSid = s
       THEN /\ eNonce' = (IF Mutant = "renonce" THEN nonce ELSE eNonce)
            /\ eLevel' = (IF Mutant = "remeasure" THEN level ELSE eLevel)
            /\ eBw' = eBw            \* a repeat keeps the session's bandwidth
            \* ...and its answer to the offer (its nonce's companion)
            /\ eOffer' = (IF Mutant = "reanswer" THEN cOffer ELSE eOffer)
            /\ eTake' = (IF Mutant = "reanswer" THEN cOffer /\ take ELSE eTake)
       ELSE /\ eNonce' = nonce /\ eLevel' = level
            /\ eBw' = cBw            \* the CALL's (min with E's own: folded into Bws)
            /\ eOffer' = cOffer /\ eTake' = (cOffer /\ take)

\* C keys a CALL; E may hear it and ACCEPT.
Call ==
    /\ cSt = "calling" /\ cCalls > 0
    /\ cCalls' = cCalls - 1
    /\ UNCHANGED <<cSt, cSid, cNonce, cLevel, cSessions, cBw, cOffer, cEnc>>
    \* take: E holds C's key and its client has not said ENCRYPT OFF
    /\ \E heard \in BOOLEAN, nonce \in Nonces, level \in Levels, take \in BOOLEAN :
        \* E connected hears C calling: C has left E's session (it stops
        \* calling before its first frame, the one that connects E), so E
        \* ends it and answers
        \* E answering, hearing C CALL another session: C has left E's (it
        \* CONNECTed again), so E answers the new one (fsm_accepting)
        LET busy == \/ eSt = "connected" /\ Mutant = "ignorecall"
                    \/ eSt = "accepting" /\ eSid # cSid /\ Mutant = "ignorenewer"
            \* a required E answers only a CALL it takes; silence is the
            \* refusal (arq_handle_incoming_connect_frame)
            refused == (EReq /\ ~(cOffer /\ take) /\ ~(EHolds /\ eSid = cSid))
                       \/ ~Takes(EMin, CMinor)
        IN
        IF ~heard \/ busy \/ refused
        THEN \* lost, or E is busy with another session: no ACCEPT
             /\ UNCHANGED <<eSt, eSid, eNonce, eLevel, eBw, sent, eLeft, eOffer, eTake>>
             /\ cPast' = cPast
             /\ deaf' = (deaf \/ (heard /\ (eLeft \/ (eSt = "accepting" /\ eSid # cSid)) /\ ~refused))
        ELSE /\ EAnswer(cSid, nonce, level, take)
             \* ACCEPTs of E's current session only: an older one's left the air
             /\ sent' = (IF EHolds /\ eSid = cSid THEN sent ELSE {})
                        \cup {[sid |-> cSid, nonce |-> eNonce', level |-> eLevel', bw |-> eBw', crypto |-> eTake']}
             /\ eLeft' = FALSE /\ deaf' = deaf
             /\ cPast' = cPast

\* C, still calling, decodes an ACCEPT of its session: any E keyed.
\* One that takes an offer C did not make is not for this CONNECT: ignored.
\* C required, an ACCEPT that did not take its offer: it hangs up (crypto_abort).
HearAccept ==
    /\ cSt = "calling"
    /\ \E a \in sent :
        /\ a.sid = cSid
        /\ (a.crypto => cOffer) \/ Mutant = "anyaccept"
        /\ Takes(CMin, EMinor)
        /\ IF CReq /\ ~a.crypto
           THEN cSt' = "idle" /\ UNCHANGED <<cNonce, cLevel, cBw, cEnc>>
           ELSE /\ cSt' = "connected" /\ cNonce' = a.nonce /\ cLevel' = a.level /\ cBw' = a.bw
                /\ cEnc' = (cOffer /\ a.crypto)
    /\ UNCHANGED <<cSid, cCalls, cSessions, eSt, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf, cPast, cOffer, eOffer, eTake>>

\* Out of CALLs: C gives up.
CallGiveUp ==
    /\ cSt = "calling" /\ cCalls = 0
    /\ cSt' = "idle"
    /\ cPast' = cPast \cup {cSid}
    /\ UNCHANGED <<cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf>> /\ UNCHANGED cryptoVars

\* Out of ACCEPT retries: E listens again, keeping the seed.
EFallback ==
    /\ eSt = "accepting"
    /\ eSt' = "fallback"
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf, cPast>> /\ UNCHANGED cryptoVars

\* The kept seed lapses (ARQ_CAR_PEER_LOST_MS after E's last ACCEPT) -- with
\* no caller connected on it: C took the ACCEPT before E sent its last, so
\* its own watchdog (PeerLost) fires first.
EExpire ==
    /\ eSt = "fallback" /\ ~(cSt = "connected" /\ cSid = eSid)
    /\ eSt' = "listening"
    /\ sent' = {} /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw>>
    /\ UNCHANGED <<eLeft, deaf, cPast>> /\ UNCHANGED cryptoVars

\* An ACCEPT of C's session reaches a C that gave up: it sends a DISCONNECT.
StaleAccept ==
    /\ cSt = "idle" /\ eSt = "accepting" /\ eSid = cSid
    /\ eSt' = "listening"
    /\ sent' = {} /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw>>
    /\ UNCHANGED <<eLeft, deaf, cPast>> /\ UNCHANGED cryptoVars

\* Another station's CALL for E while E answers C's: ignored -- but with the
\* bandwidth leak, it set the session bandwidth E's next ACCEPT says.
ForeignCall ==
    /\ eSt = "accepting"
    /\ \E b \in Bws : eBw' = (IF Mutant = "bwleak" THEN b ELSE eBw)
    /\ sent' = {[a EXCEPT !.bw = eBw'] : a \in sent}
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel>>
    /\ UNCHANGED <<eLeft, deaf, cPast>> /\ UNCHANGED cryptoVars

\* C's first round reaches E: on the rung E named, with E's nonce in its CRC.
\* (On another bandwidth the ends cap their rungs and pick their floor apart:
\* NoDoom catches it.)
FirstFrame ==
    /\ cSt = "connected" /\ EHolds /\ eSid = cSid
    /\ cNonce = eNonce /\ cLevel = eLevel
    /\ eSt' = "connected"
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf, cPast>> /\ UNCHANGED cryptoVars

\* C connected, E never: C's watchdog fires (ARQ_CAR_PEER_LOST_MS after the
\* ACCEPT it took).  E's kept seed lapses later (EExpire); until then C may
\* CONNECT again and E answer it from the fallback -- the same ACCEPT if C
\* picked the same id (trace validation found the code doing this, sim
\* seed 7: the two lapsed in one step here before 2026-11-23).
PeerLost ==
    /\ cSt = "connected" /\ eSt # "connected"
    /\ cSt' = "idle"
    /\ cPast' = cPast \cup {cSid}
    /\ UNCHANGED <<cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent>>
    /\ UNCHANGED <<eLeft, deaf>> /\ UNCHANGED cryptoVars

\* Both connected, C's watchdog fires first: E stays connected, in a session
\* C has left.
CLinkLost ==
    /\ cSt = "connected" /\ eSt = "connected" /\ eSid = cSid /\ ~eLeft
    /\ cSt' = "idle" /\ eLeft' = TRUE
    /\ cPast' = cPast \cup {cSid}
    /\ UNCHANGED <<cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSt, eSid, eNonce, eLevel, eBw, sent, deaf>>
    /\ UNCHANGED cryptoVars

\* ...and then E's.
ELinkLost ==
    /\ eSt = "connected" /\ eLeft
    /\ eSt' = "listening" /\ eLeft' = FALSE /\ sent' = {}
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, eSid, eNonce, eLevel, eBw, deaf, cPast>>
    /\ UNCHANGED cryptoVars

\* A CALL C keyed for a session it has since left surfaces at E (a decoder
\* backlog).  E, answering, ignores it: it is too old to answer, and C is
\* not calling that session -- answering it (Mutant "staletake": the switch
\* before the age check) leaves the session being answered, whose ACCEPT C
\* may have taken, and answers a dead one.
StaleCall ==
    /\ eSt = "accepting"
    /\ \E s \in cPast \ {cSid}, nonce \in Nonces, level \in Levels, take \in BOOLEAN :
        IF Mutant = "staletake"
        THEN /\ EAnswer(s, nonce, level, take)
             /\ sent' = {[sid |-> s, nonce |-> eNonce', level |-> eLevel', bw |-> eBw', crypto |-> eTake']}
             /\ eLeft' = FALSE
        ELSE UNCHANGED <<eSt, eSid, eNonce, eLevel, eBw, sent, eLeft, eOffer, eTake>>
    /\ UNCHANGED <<cSt, cSid, cCalls, cNonce, cLevel, cSessions, cBw, deaf, cPast>>
    /\ UNCHANGED <<cOffer, cEnc>>

\* Over: both connected, or both idle with the application done connecting.
Done == /\ \/ cSt = "connected" /\ eSt = "connected"
           \/ cSt = "idle" /\ cSessions = S /\ ~EHolds
        /\ UNCHANGED vars

Next == AppConnect \/ Call \/ HearAccept \/ CallGiveUp \/ EFallback \/ EExpire \/ StaleAccept
        \/ ForeignCall \/ FirstFrame \/ PeerLost \/ CLinkLost \/ ELinkLost \/ StaleCall \/ Done

\* Timers run out and the channel is not an adversary for ever: C's first
\* round, possible again and again, gets through at last.
Spec == Init /\ [][Next]_vars
        /\ WF_vars(Call) /\ WF_vars(CallGiveUp) /\ WF_vars(EExpire) /\ WF_vars(PeerLost) /\ WF_vars(EFallback)
        /\ WF_vars(ELinkLost)
        /\ SF_vars(FirstFrame)

\* ---- properties ----
TypeOK ==
    /\ cSt \in {"idle", "calling", "connected"} /\ cSid \in Sids \cup {0}
    /\ cBw \in Bws \cup {0} /\ eBw \in Bws \cup {0}
    /\ cCalls \in 0 .. R /\ cSessions \in 0 .. S
    /\ cNonce \in Nonces \cup {0} /\ cLevel \in Levels \cup {0}
    /\ eSt \in {"listening", "accepting", "fallback", "connected"} /\ eSid \in Sids \cup {0}
    /\ eNonce \in Nonces \cup {0} /\ eLevel \in Levels \cup {0}
    /\ eLeft \in BOOLEAN /\ deaf \in BOOLEAN /\ cPast \subseteq Sids
    /\ cOffer \in BOOLEAN /\ cEnc \in BOOLEAN /\ eOffer \in BOOLEAN /\ eTake \in BOOLEAN

NoDoom == (cSt = "connected" /\ (EHolds \/ eSt = "connected") /\ eSid = cSid) =>
              (cNonce = eNonce /\ cLevel = eLevel /\ cBw = eBw)
NoHalfConnect == eSt = "connected" /\ ~eLeft => (cSt = "connected" /\ cSid = eSid /\ cNonce = eNonce /\ cLevel = eLevel)
NoDeafRedial == ~deaf
\* E answering (or holding the seed of) a session its connected caller is
\* not in: the caller's frames fail the seed, and the session dies.  The
\* stale-CALL switch ("staletake") reaches this state; Settles cannot see
\* it -- a lost-peer failure still ends with both idle.
NoDeadAnswer == ~(cSt = "connected" /\ EHolds /\ eSid # cSid)
Settles == <>[]((cSt = "connected" /\ eSt = "connected") \/ (cSt = "idle" /\ ~EHolds))
Agree == eSt = "connected" /\ ~eLeft => cEnc = (eOffer /\ eTake)
ReqGate == /\ (CReq /\ cSt = "connected") => cEnc
           /\ (EReq /\ eSt = "connected") => (eOffer /\ eTake)
SameLadder == cSt = "connected" => Ladder(CMinor) = Ladder(EMinor)
=============================================================================
