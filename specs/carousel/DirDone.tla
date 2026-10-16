------------------------------- MODULE DirDone ---------------------------------
(***************************************************************************)
(* When may an end conclude that its peer's direction is done -- what a  *)
(* DISCONNECT waits for (car_is_drained: mine retired, peer_direction_done)*)
(* -- in datalink_arq/carousel.c:                                         *)
(*                                                                         *)
(*   P, the peer, gets data from its application at any time, cuts it     *)
(*   into blocks (opening them), and sends them; every frame of its says  *)
(*   what it holds: the highest block opened and whether data waits to be *)
(*   cut (a STATUS does, and a poll since arch-2.0).                      *)
(*                                                                         *)
(*   Q, the end that decides, delivers P's blocks in order (rbase) and    *)
(*   keeps P's last report.  P's direction is done (peer_direction_done)  *)
(*   when a STATUS said so since Q's last poll (status_seen), or P's last *)
(*   report has nothing uncut and nothing opened above rbase.            *)
(*                                                                         *)
(*   status_seen is also set without a STATUS: Q offered P its turn       *)
(*   (KD_BOTH, only to a P whose direction is done: offer_both) and       *)
(*   heard nothing back, or a pattern -- which may be a false alarm, or  *)
(*   another station's -- or a poll with no round that has no data.       *)
(*                                                                         *)
(* Delivery, rounds and the channel are abstracted: P's round delivers    *)
(* some of its blocks or none; any frame may be lost.                     *)
(*                                                                         *)
(* Property: Sound -- whenever Q takes P's direction as done, everything  *)
(* P held as of its last frame Q heard has been delivered.  (What P's     *)
(* application writes after that, Q cannot know: a DISCONNECT the          *)
(* application asked for meets it as it would on any link.)              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS NB,      \* blocks P's application may produce
          Mutant   \* "none", or one rule broken (to show the model can fail):
                   \*   "offerany":   Q offers P its turn whatever it knows
                   \*   "keepstatus": a round of P's leaves status_seen set (the
                   \*                 code before arch-2.0: until Q's next poll)

VARIABLES
    pOpened,    \* P: blocks opened (ids 0 .. pOpened - 1)
    pUncut,     \* P: data written, not yet in a block
    pWrites,    \* P: application writes so far
    rbase,      \* Q: blocks delivered
    qHi,        \* Q: P's last report, blocks opened
    qUncut,     \* Q: ...data uncut
    statusSeen, \* Q: status_seen
    hOpened, hUncut   \* ghost: what P held at the last frame Q heard

vars == <<pOpened, pUncut, pWrites, rbase, qHi, qUncut, statusSeen, hOpened, hUncut>>

Init == pOpened = 0 /\ pUncut = FALSE /\ pWrites = 0 /\ rbase = 0
        /\ qHi = 0 /\ qUncut = FALSE /\ statusSeen = FALSE /\ hOpened = 0 /\ hUncut = FALSE

Done == statusSeen \/ (~qUncut /\ qHi <= rbase)          \* peer_direction_done
PHasData == pUncut \/ pOpened > rbase                     \* P knows its acks (abstracted)

\* Q hears a frame of P's: its report, and the ghost of what P held.
Heard == qHi' = pOpened /\ qUncut' = pUncut /\ hOpened' = pOpened /\ hUncut' = pUncut

\* P's application writes; P cuts a block.
Write == pWrites < NB /\ pWrites' = pWrites + 1 /\ pUncut' = TRUE
         /\ UNCHANGED <<pOpened, rbase, qHi, qUncut, statusSeen, hOpened, hUncut>>
Cut == pUncut /\ pOpened < pWrites
       /\ pOpened' = pOpened + 1
       /\ pUncut' \in (IF pOpened + 1 < pWrites THEN {TRUE} ELSE {FALSE})
       /\ UNCHANGED <<pWrites, rbase, qHi, qUncut, statusSeen, hOpened, hUncut>>

\* P's round reaches Q: some of its blocks decode.
\* (take_pieces: a round clears status_seen -- P had something after all.)
RoundIn == /\ \E n \in rbase .. pOpened : rbase' = n
           /\ Heard
           /\ statusSeen' = (IF Mutant = "keepstatus" THEN statusSeen ELSE FALSE)
           /\ UNCHANGED <<pOpened, pUncut, pWrites>>
Round == pOpened > rbase /\ RoundIn

\* Q polls P: status_seen is cleared (send_poll, m_send_req).
QPoll == statusSeen' = FALSE
         /\ UNCHANGED <<pOpened, pUncut, pWrites, rbase, qHi, qUncut, hOpened, hUncut>>

\* P, with nothing to send, answers with a STATUS.
Status == ~PHasData
          /\ statusSeen' = TRUE /\ Heard
          /\ UNCHANGED <<pOpened, pUncut, pWrites, rbase>>

\* Q offers P its turn (KD_BOTH); P answers with its round, a pattern or a poll.
Offer == (Done \/ Mutant = "offerany")
         /\ \/ \* nothing heard back: status_seen (m_decide_recv)
               /\ statusSeen' = TRUE
               /\ UNCHANGED <<pOpened, pUncut, pWrites, rbase, qHi, qUncut, hOpened, hUncut>>
            \/ \* a pattern -- P's, or a false alarm: status_seen (car_on_pattern)
               /\ statusSeen' = TRUE
               /\ UNCHANGED <<pOpened, pUncut, pWrites, rbase, qHi, qUncut, hOpened, hUncut>>
            \/ \* a poll with no round and no data (m_on_ctl): P had nothing
               /\ ~PHasData /\ statusSeen' = TRUE /\ Heard
               /\ UNCHANGED <<pOpened, pUncut, pWrites, rbase>>
            \/ \* P's round: as Round
               pOpened > rbase /\ RoundIn

Next == Write \/ Cut \/ Round \/ QPoll \/ Status \/ Offer

Spec == Init /\ [][Next]_vars

TypeOK == pOpened \in 0 .. NB /\ pWrites \in 0 .. NB /\ rbase \in 0 .. NB /\ qHi \in 0 .. NB
Sound == Done => (rbase >= hOpened /\ ~hUncut)
=============================================================================
