---------------------------- MODULE CarouselProof -----------------------------
(***************************************************************************)
(* The carousel's block layer, one direction, in the form TLAPS proves.    *)
(* CarouselBlocks.tla is the model TLC explores; this one says the same    *)
(* things with sets and functions over block ids (no recursion, no         *)
(* SelectSeq), and with ghost fields on the frames in flight, so that its  *)
(* safety can be proved for any number of blocks, any K and any channel.  *)
(*                                                                         *)
(* It over-approximates the code where safety does not depend on it:       *)
(*   - the sender may send a piece of ANY open block at any time.  A BREAK *)
(*     (genuine, spurious or another station's) only changes which block   *)
(*     the sender sends next, so BREAKs are covered without being modelled;*)
(*   - a block may decode on any piece of it (K is irrelevant to safety);  *)
(*   - nothing is cleared by half duplex, and any frame may be lost.       *)
(* It keeps what safety does depend on: the wire carries block ids mod     *)
(* MOD = 2*WIN; the receiver resolves them against its base; a poll       *)
(* reports its base mod MOD and, per window slot, whether the block is     *)
(* still needed; the sender retires a block whose slot says "not needed"   *)
(* or that lies behind the base; it opens a block only while its open      *)
(* blocks span fewer than WIN ids; each direction is a FIFO.               *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS WIN,  \* the window (8 in the code); ids travel mod 2*WIN
          Ids   \* block ids: Nat for the proof, 0..n for TLC

ASSUME WinAssm == WIN \in Nat \ {0}
ASSUME IdsAssm == Ids \subseteq Nat /\ 0 \in Ids
MOD == 2 * WIN

VARIABLES
    open,     \* sender: open (unretired) block ids
              \* (functions below are over Ids: Nat for the proof)
    nxt,      \* sender: the next id to open
    down,     \* data frames in flight, a FIFO: [wire, true, snxt]
              \*   true = the block's id (ghost), snxt = nxt when sent (ghost)
    up,       \* polls in flight, a FIFO: [base, need, abase, sd]
              \*   abase = the receiver's base when sent, sd = its decoded
              \*   blocks then (ghosts)
    rbase,    \* receiver: the first block not decoded
    done,     \* receiver: block id -> decoded
    got,      \* receiver: block id -> true ids of the pieces filed under it (ghost)
    retired   \* ghost: blocks the sender has dropped

vars == <<open, nxt, down, up, rbase, done, got, retired>>

Slots == 0 .. WIN - 1
Data  == [wire : 0 .. MOD - 1, true : Nat, snxt : Nat]
Poll  == [base : 0 .. MOD - 1, need : [Slots -> BOOLEAN], abase : Nat, sd : [Ids -> BOOLEAN]]

Init ==
    /\ open = {} /\ nxt = 0 /\ down = << >> /\ up = << >>
    /\ rbase = 0 /\ done = [b \in Ids |-> FALSE] /\ got = [b \in Ids |-> {}]
    /\ retired = {}

(* ---- sender ---- *)
Open ==
    /\ nxt + MOD \in Ids          \* room for the window above it (Ids = Nat: always)
    /\ \A b \in open : nxt - b < WIN
    /\ open' = open \cup {nxt}
    /\ nxt' = nxt + 1
    /\ UNCHANGED <<down, up, rbase, done, got, retired>>

SendPiece(b) ==
    /\ b \in open
    /\ down' = Append(down, [wire |-> b % MOD, true |-> b, snxt |-> nxt])
    /\ UNCHANGED <<open, nxt, up, rbase, done, got, retired>>

\* apply_need: a block is kept only if it lies in the poll's window and its
\* slot says it is still needed.
Keep(b, m) == LET off == ((b % MOD) + MOD - m.base) % MOD
              IN  off < WIN /\ m.need[off]

RecvUp ==
    /\ up # << >>
    /\ LET m == Head(up) IN
         /\ open' = {b \in open : Keep(b, m)}
         /\ retired' = retired \cup {b \in open : ~Keep(b, m)}
    /\ up' = Tail(up)
    /\ UNCHANGED <<nxt, down, rbase, done, got>>

(* ---- receiver ---- *)
SendPoll ==
    /\ up' = Append(up, [base  |-> rbase % MOD,
                         need  |-> [o \in Slots |-> ~done[rbase + o]],
                         abase |-> rbase,
                         sd    |-> done])
    /\ UNCHANGED <<open, nxt, down, rbase, done, got, retired>>

\* take_pieces + decode + deliver_in_order: a piece resolved into the window
\* is filed under its block, which may then decode; the base slides to the
\* first block not decoded.  Behind the base (off >= WIN) or decoded: dropped.
Abs(m) == rbase + ((m.wire + MOD - (rbase % MOD)) % MOD)
InWin(m) == (m.wire + MOD - (rbase % MOD)) % MOD < WIN

RecvDown ==
    /\ down # << >>
    /\ LET m == Head(down) IN
       IF InWin(m) /\ ~done[Abs(m)]
       THEN \E dec \in BOOLEAN, x \in Ids :
              /\ got' = [got EXCEPT ![Abs(m)] = @ \cup {m.true}]
              /\ done' = [done EXCEPT ![Abs(m)] = dec]
              \* the base slides to the first block not decoded (one always
              \* exists: nxt is never decoded)
              /\ x >= rbase /\ ~done'[x] /\ \A y \in rbase .. x - 1 : done'[y]
              /\ rbase' = x
       ELSE UNCHANGED <<got, done, rbase>>
    /\ down' = Tail(down)
    /\ UNCHANGED <<open, nxt, up, retired>>

(* ---- channel ---- *)
LoseDown == /\ down # << >> /\ down' = Tail(down)
            /\ UNCHANGED <<open, nxt, up, rbase, done, got, retired>>
LoseUp   == /\ up # << >> /\ up' = Tail(up)
            /\ UNCHANGED <<open, nxt, down, rbase, done, got, retired>>

Next ==
    \/ Open
    \/ \E b \in open : SendPiece(b)
    \/ RecvUp \/ SendPoll \/ RecvDown \/ LoseDown \/ LoseUp

Spec == Init /\ [][Next]_vars

(* ---- safety ---- *)
\* A block leaves the sender only once the receiver has decoded it.
RetireSafe == \A b \in retired : done[b]
\* Every block below the base was rebuilt only from its own pieces; the
\* blocks reach the application in id order (the base only slides over
\* decoded blocks), so this is the delivered stream's integrity.
Integrity == \A b \in Ids : got[b] \subseteq {b}
Safety == RetireSafe /\ Integrity

(* ---- the inductive invariant ---- *)
TypeOK ==
    /\ open \subseteq Nat /\ nxt \in Nat /\ rbase \in Nat /\ retired \subseteq Nat
    /\ down \in Seq(Data) /\ up \in Seq(Poll)
    /\ done \in [Ids -> BOOLEAN] /\ got \in [Ids -> SUBSET Nat]

\* the sender
S1 == \A b \in open : b < nxt /\ nxt - b <= WIN
S2 == \A b \in 0 .. nxt - 1 : b \in open \/ b \in retired
S3 == \A b \in retired : b < nxt
\* the receiver
R1 == \A b \in 0 .. rbase - 1 : done[b]
R2 == ~done[rbase]
R3 == \A b \in Ids : done[b] => b < nxt
\* data in flight: sent from an open block, resolvable against the base
D1 == \A i \in 1 .. Len(down) :
        /\ down[i].wire = down[i].true % MOD
        /\ down[i].true < down[i].snxt /\ down[i].snxt <= nxt
        /\ down[i].snxt <= down[i].true + WIN
        /\ down[i].true < rbase + WIN
D2 == \A i, j \in 1 .. Len(down) : i < j => down[i].snxt <= down[j].snxt
D3 == \A i \in 1 .. Len(down) : \A b \in Ids : done[b] => b < down[i].snxt
\* polls in flight: a faithful snapshot, older ones first
P1 == \A i \in 1 .. Len(up) :
        /\ up[i].base = up[i].abase % MOD
        /\ up[i].abase <= rbase
        /\ \A b \in Ids : up[i].sd[b] => done[b]
        /\ \A b \in 0 .. up[i].abase - 1 : up[i].sd[b]
        /\ ~up[i].sd[up[i].abase]
        /\ \A o \in Slots : up[i].need[o] = ~up[i].sd[up[i].abase + o]
P2 == \A i, j \in 1 .. Len(up) : i < j =>
        /\ up[i].abase <= up[j].abase
        /\ \A b \in Ids : up[i].sd[b] => up[j].sd[b]
P3 == \A i \in 1 .. Len(up) : up[i].abase \in open \/ up[i].abase >= nxt
P4 == \A i \in 1 .. Len(up) : \A b \in open : b < up[i].abase + WIN

Inv == /\ TypeOK /\ Safety
       /\ S1 /\ S2 /\ S3 /\ R1 /\ R2 /\ R3
       /\ D1 /\ D2 /\ D3 /\ P1 /\ P2 /\ P3 /\ P4
=============================================================================
