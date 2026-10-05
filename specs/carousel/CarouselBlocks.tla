---------------------------- MODULE CarouselBlocks ----------------------------
(***************************************************************************)
(* The carousel's block layer, one direction: a sender opens blocks, sends *)
(* their pieces, and retires them on the receiver's word; the receiver     *)
(* collects pieces into blocks by wire id, decodes, delivers in order, and *)
(* reports by poll (base + need per window slot) and, at the floor, by an  *)
(* anonymous BREAK pattern.  Mirrors datalink_arq/carousel.c:              *)
(*   ids on the wire mod MOD, window WIN, MOD = 2*WIN  (16 and 8 in code); *)
(*   take_pieces: off = (wire - rbase) mod MOD; off >= WIN or a decoded    *)
(*     block means "delivered already" (sets the BREAK flag);              *)
(*   apply_need: off = (id - base) mod MOD; off >= WIN or need 0 retires;  *)
(*   car_on_pattern: a floor BREAK names floor_blk; if at least K of its   *)
(*     pieces went out the sender stops sending it, but only a poll        *)
(*     retires it (a BREAK is advisory);                                   *)
(*   a floor round carries one block: the oldest one not stopped;          *)
(*   a block is opened only while the id span is < WIN.                    *)
(* Time, rungs and turns are abstracted away: any enabled action may run,  *)
(* which over-approximates every schedule the timers can produce.          *)
(* Each piece carries its true block id as a ghost field, so a block       *)
(* rebuilt from another block's piece is visible.                          *)
(*                                                                         *)
(* SPURIOUS: a BREAK the receiver did not send (a detector false alarm, or *)
(*   another station's pattern -- patterns carry no session).              *)
(* STALE: a frame of an earlier session with the same CRC seed (the        *)
(*   session id has 64 values), carrying any block id.                     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS NB,        \* blocks to send
          K,         \* distinct pieces that decode a block
          SENTMAX,   \* distinct pieces a sender sends of a block (K + repair)
          MOD, WIN,  \* wire id modulus and window (MOD = 2 * WIN)
          CHAN,      \* most frames in flight
          FLOOR,     \* the sender streams the floor: one block per round, BREAK accepted
          SPURIOUS,  \* allow a BREAK nobody sent
          STALE      \* allow a stale same-seed frame with an arbitrary id

ASSUME MOD = 2 * WIN /\ K <= SENTMAX

VARIABLES
    sb,        \* sender: open blocks [id |-> abs id, sent |-> pieces sent, stop |-> BREAKed]
    nxt,       \* sender: next block id to open
    floorBlk,  \* sender: the block its last floor round carried
    down,      \* frames in flight sender -> receiver, in order (a FIFO: no overtaking on air)
    up,        \* frames in flight receiver -> sender, in order
    rbase,     \* receiver: lowest undelivered block (abs)
    pcs,       \* receiver: abs id -> piece indices held (the code's got[])
    got,       \* receiver: abs id -> TRUE block ids those pieces came from (ghost)
    done,      \* receiver: abs id -> decoded
    brk,       \* receiver: something to BREAK about since its last answer
    delivered, \* receiver: blocks handed to the application, in order
    retired    \* ghost: blocks the sender has dropped

vars == <<sb, nxt, floorBlk, down, up, rbase, pcs, got, done, brk, delivered, retired>>
rxv  == <<rbase, pcs, got, done, brk, delivered>>
txv  == <<sb, nxt, floorBlk, retired>>

STALEID == NB + MOD            \* the true id of a stale frame: no real block
Ids == 0 .. NB + 2 * MOD

Init ==
    /\ sb = << >> /\ nxt = 0 /\ floorBlk = 0 /\ down = << >> /\ up = << >>
    /\ rbase = 0 /\ pcs = [b \in Ids |-> {}] /\ got = [b \in Ids |-> {}]
    /\ done = [b \in Ids |-> FALSE] /\ brk = FALSE /\ delivered = << >>
    /\ retired = {}

(* ---- sender ---- *)
Span == IF sb = << >> THEN 0 ELSE nxt - sb[1].id

Open ==
    /\ nxt < NB /\ Span < WIN
    /\ sb' = Append(sb, [id |-> nxt, sent |-> 0, stop |-> FALSE])
    /\ nxt' = nxt + 1
    /\ UNCHANGED <<floorBlk, down, up, retired>> /\ UNCHANGED rxv

\* The block a floor round carries: the oldest not stopped by a BREAK, or the
\* oldest of all when every one is (its repair, until a poll comes).
Live == {j \in 1 .. Len(sb) : ~sb[j].stop}
FloorIdx == IF Live = {} THEN 1 ELSE CHOOSE j \in Live : \A k \in Live : j <= k

SendPiece(i) ==
    /\ i \in 1 .. Len(sb)
    /\ FLOOR => i = FloorIdx
    /\ sb[i].sent < SENTMAX
    /\ Len(down) < CHAN
    /\ down' = Append(down, [t |-> "data", wire |-> sb[i].id % MOD, true |-> sb[i].id,
                             piece |-> sb[i].sent])
    /\ sb' = [sb EXCEPT ![i].sent = @ + 1]
    /\ floorBlk' = IF FLOOR THEN sb[i].id ELSE floorBlk
    \* Half duplex: frames still on the air towards a station that keys are
    \* lost -- it cannot hear while it transmits.  (On air a frame arrives as
    \* it is sent; one the sender has not taken yet is one it keyed over.)
    /\ up' = << >>
    /\ UNCHANGED <<nxt, retired>> /\ UNCHANGED rxv

Keep(blk, m) == LET off == (blk.id + MOD - m.base) % MOD
                IN off < WIN /\ m.need[off] # 0

\* A poll retires what it reports done, and a block it reports still needed
\* is sent again even if a BREAK stopped it.
OnPoll(m) ==
    /\ LET kept == SelectSeq(sb, LAMBDA blk : Keep(blk, m))
       IN sb' = [j \in 1 .. Len(kept) |-> [kept[j] EXCEPT !.stop = FALSE]]
    /\ retired' = retired \cup {sb[i].id : i \in {j \in 1 .. Len(sb) : ~Keep(sb[j], m)}}
    /\ UNCHANGED <<nxt, floorBlk>>

OnBreak ==
    IF FLOOR /\ \E j \in 1 .. Len(sb) : sb[j].id = floorBlk /\ sb[j].sent >= K
    THEN /\ sb' = [j \in 1 .. Len(sb) |->
                      IF sb[j].id = floorBlk /\ sb[j].sent >= K
                      THEN [sb[j] EXCEPT !.stop = TRUE] ELSE sb[j]]
         /\ UNCHANGED <<nxt, floorBlk, retired>>
    ELSE UNCHANGED txv

(* ---- receiver ---- *)
OnData(m) ==
    LET off == (m.wire + MOD - (rbase % MOD)) % MOD
        abs == rbase + off
    IN IF off >= WIN \/ done[abs]
       THEN /\ brk' = TRUE
            /\ UNCHANGED <<pcs, got, done>>
       ELSE IF m.piece \in pcs[abs]
            THEN UNCHANGED <<pcs, got, done, brk>>
            ELSE /\ pcs' = [pcs EXCEPT ![abs] = @ \cup {m.piece}]
                 /\ got' = [got EXCEPT ![abs] = @ \cup {m.true}]
                 /\ IF Cardinality(pcs[abs]) + 1 >= K
                    THEN /\ done' = [done EXCEPT ![abs] = TRUE]
                         /\ brk' = TRUE
                    ELSE UNCHANGED <<done, brk>>

\* deliver_in_order, run by on_data right after the pieces are taken: the base
\* slides over every decoded block at once, so a poll's base is always the
\* first block not yet decoded.
RECURSIVE Contig(_, _)
Contig(d, b) == IF b < NB + 2 * MOD - WIN /\ d[b] THEN 1 + Contig(d, b + 1) ELSE 0
Slide ==
    LET n == Contig(done', rbase)
    IN /\ rbase' = rbase + n
       /\ delivered' = delivered \o [i \in 1 .. n |-> rbase + i - 1]

PollMsg == [t |-> "poll", base |-> rbase % MOD,
            need |-> [o \in 0 .. WIN - 1 |-> IF done[rbase + o] THEN 0 ELSE K]]

SendPoll ==
    /\ Len(up) < CHAN
    /\ up' = Append(up, PollMsg)
    /\ down' = << >>
    /\ brk' = FALSE                  \* send_poll clears rx_break
    /\ UNCHANGED <<rbase, pcs, got, done, delivered>> /\ UNCHANGED txv

SendBreak ==
    /\ FLOOR /\ brk
    /\ Len(up) < CHAN
    /\ up' = Append(up, [t |-> "break"])
    /\ brk' = FALSE
    /\ down' = << >>
    /\ UNCHANGED <<rbase, pcs, got, done, delivered>> /\ UNCHANGED txv

(* ---- channel ---- *)
\* The head of a queue arrives, or is lost; nothing overtakes.
RecvDown ==
    /\ down # << >>
    /\ down' = Tail(down)
    /\ OnData(Head(down)) /\ Slide /\ UNCHANGED up /\ UNCHANGED txv

RecvUp ==
    /\ up # << >>
    /\ up' = Tail(up)
    /\ CASE Head(up).t = "poll"  -> OnPoll(Head(up)) /\ UNCHANGED rxv
         [] Head(up).t = "break" -> OnBreak /\ UNCHANGED rxv
    /\ UNCHANGED down

LoseDown == /\ down # << >> /\ down' = Tail(down) /\ UNCHANGED up /\ UNCHANGED rxv /\ UNCHANGED txv
LoseUp   == /\ up # << >> /\ up' = Tail(up) /\ UNCHANGED down /\ UNCHANGED rxv /\ UNCHANGED txv

InjectBreak ==
    /\ SPURIOUS /\ Len(up) < CHAN
    /\ up' = Append(up, [t |-> "break"])
    /\ UNCHANGED down /\ UNCHANGED rxv /\ UNCHANGED txv

InjectStale(w) ==
    /\ STALE /\ Len(down) < CHAN
    /\ down' = Append(down, [t |-> "data", wire |-> w, true |-> STALEID, piece |-> 0])
    /\ UNCHANGED up /\ UNCHANGED rxv /\ UNCHANGED txv

Next ==
    \/ Open
    \/ \E i \in 1 .. Len(sb) : SendPiece(i)
    \/ SendPoll \/ SendBreak
    \/ RecvDown \/ RecvUp \/ LoseDown \/ LoseUp
    \/ InjectBreak
    \/ \E w \in 0 .. MOD - 1 : InjectStale(w)

Spec == Init /\ [][Next]_vars

(* ---- properties ---- *)
\* Delivered in order, each block exactly once.
InOrder == \A i \in 1 .. Len(delivered) : delivered[i] = i - 1

\* Every delivered block was rebuilt only from its own pieces.
Integrity == \A i \in 1 .. Len(delivered) : got[delivered[i]] \subseteq {delivered[i]}

\* A block leaves the sender only once the receiver holds it decoded:
\* otherwise nobody will ever send it again.
RetireSafe == \A b \in retired : b < NB => done[b]

\* Not a safety property: checked as an invariant it must FAIL, which shows a
\* complete transfer is reachable (the model is not safe by doing nothing).
NotFinished == Len(delivered) < NB

\* Nothing past the real blocks is ever delivered.
NoPhantom == \A i \in 1 .. Len(delivered) : delivered[i] < NB
=============================================================================
