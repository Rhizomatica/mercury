--------------------------- MODULE CarouselProofs ----------------------------
(***************************************************************************)
(* TLAPS proof that CarouselProof's block layer is safe: a block leaves     *)
(* the sender only once the receiver has decoded it, and every block is     *)
(* rebuilt only from its own pieces -- for any number of blocks, any K,     *)
(* any channel length and loss pattern, with the code's window: WIN = 8,    *)
(* ids mod 16.  (With a symbolic WIN, % is non-linear for the SMT solver;   *)
(* only the window lemmas below use it.)                                    *)
(*                                                                          *)
(*   tlapm --threads 8 CarouselProofs.tla                                   *)
(***************************************************************************)
EXTENDS CarouselProof, TLAPS, SequenceTheorems

ASSUME IdsNat == Ids = Nat
ASSUME Win8   == WIN = 8

(* ---- the window: residues mod 2*WIN resolve within WIN of the base ---- *)
LEMMA ResolveIn ==
  \A x, r \in Nat : r <= x /\ x < r + WIN => ((x % MOD) + MOD - (r % MOD)) % MOD = x - r
  BY Win8 DEF MOD

LEMMA ResolveBehindA ==
  ASSUME NEW x \in Nat, NEW r \in Nat, x < r, x + WIN >= r
  PROVE  ((x % MOD) + MOD - (r % MOD)) % MOD >= WIN
  BY Win8 DEF MOD

LEMMA ResolveBehind ==
  \A x, r \in Nat : x < r /\ x + WIN >= r => ((x % MOD) + MOD - (r % MOD)) % MOD >= WIN
  BY ResolveBehindA

LEMMA ModRange ==
  ASSUME NEW x \in Nat
  PROVE  x % MOD \in 0 .. MOD - 1
  BY Win8 DEF MOD

(* ---- types: what the solvers need to be told about sequences ---- *)
LEMMA SeqTypes ==
  ASSUME TypeOK
  PROVE  /\ Len(down) \in Nat /\ Len(up) \in Nat
         /\ \A i \in 1 .. Len(down) : down[i] \in Data
         /\ \A i \in 1 .. Len(up) : up[i] \in Poll
  BY LenProperties, ElementOfSeq DEF TypeOK

(* ---- initial state ---- *)
LEMMA InitInv == Init => Inv
  BY IdsNat, WinAssm DEF Init, Inv, TypeOK, Safety, RetireSafe, Integrity,
     S1, S2, S3, R1, R2, R3, D1, D2, D3, P1, P2, P3, P4

(* ---- the simple actions ---- *)
LEMMA LoseDownInv == Inv /\ LoseDown => Inv'
  <1> SUFFICES ASSUME Inv, LoseDown PROVE Inv' OBVIOUS
  <1>1. down \in Seq(Data) /\ down # << >> BY DEF Inv, TypeOK, LoseDown
  <1>2. /\ Tail(down) \in Seq(Data)
        /\ Len(Tail(down)) = Len(down) - 1
        /\ \A i \in 1 .. Len(Tail(down)) : Tail(down)[i] = down[i+1]
    BY <1>1, HeadTailProperties
  <1> QED
    BY <1>2, IdsNat DEF LoseDown, Inv, TypeOK, Safety, RetireSafe, Integrity,
       S1, S2, S3, R1, R2, R3, D1, D2, D3, P1, P2, P3, P4

LEMMA LoseUpInv == Inv /\ LoseUp => Inv'
  <1> SUFFICES ASSUME Inv, LoseUp PROVE Inv' OBVIOUS
  <1>1. up \in Seq(Poll) /\ up # << >> BY DEF Inv, TypeOK, LoseUp
  <1>2. /\ Tail(up) \in Seq(Poll)
        /\ Len(Tail(up)) = Len(up) - 1
        /\ \A i \in 1 .. Len(Tail(up)) : Tail(up)[i] = up[i+1]
    BY <1>1, HeadTailProperties
  <1> QED
    BY <1>2, IdsNat DEF LoseUp, Inv, TypeOK, Safety, RetireSafe, Integrity,
       S1, S2, S3, R1, R2, R3, D1, D2, D3, P1, P2, P3, P4

LEMMA SendPieceInv == ASSUME NEW b \in open PROVE Inv /\ SendPiece(b) => Inv'
  <1> SUFFICES ASSUME Inv, SendPiece(b) PROVE Inv' OBVIOUS
  <1> DEFINE f == [wire |-> b % MOD, true |-> b, snxt |-> nxt]
  <1>0. b \in Nat /\ nxt \in Nat /\ rbase \in Nat BY DEF Inv, TypeOK
  <1>1. f \in Data BY <1>0, ModRange DEF Data
  <1>2. down \in Seq(Data) BY DEF Inv, TypeOK
  <1>3. /\ Append(down, f) \in Seq(Data)
        /\ Len(Append(down, f)) = Len(down) + 1
        /\ \A i \in 1 .. Len(down) : Append(down, f)[i] = down[i]
        /\ Append(down, f)[Len(down) + 1] = f
    BY <1>1, <1>2, AppendProperties
  \* the new frame: its block is open, so within WIN below nxt, and nxt is
  \* within WIN above the base (the base is open, or past every open block)
  <1>4. b < nxt /\ nxt - b <= WIN BY DEF Inv, S1
  <1>5. rbase \in open \/ rbase >= nxt
    <2>1. ~done[rbase] BY DEF Inv, R2
    <2>2. rbase \notin retired BY <2>1 DEF Inv, Safety, RetireSafe
    <2>3. CASE rbase < nxt BY <2>2, <1>0, <2>3 DEF Inv, S2
    <2> QED BY <2>3, <1>0
  <1>6. b < rbase + WIN
    <2>1. CASE rbase >= nxt BY <2>1, <1>4, <1>0, WinAssm
    <2>2. CASE rbase \in open BY <2>2, <1>4, <1>0, WinAssm DEF Inv, S1
    <2> QED BY <1>5, <2>1, <2>2
  <1>7. \A x \in Ids : done[x] => x < nxt BY DEF Inv, R3
  <1>8. \A i \in 1 .. Len(down) : down[i].snxt <= nxt BY DEF Inv, D1
  <1> QED
    BY <1>0, <1>3, <1>4, <1>6, <1>7, <1>8, IdsNat, WinAssm
       DEF SendPiece, Inv, TypeOK, Safety, RetireSafe, Integrity,
           S1, S2, S3, R1, R2, R3, D1, D2, D3, P1, P2, P3, P4

LEMMA SendPollInv == Inv /\ SendPoll => Inv'
  <1> SUFFICES ASSUME Inv, SendPoll PROVE Inv' OBVIOUS
  <1> DEFINE p == [base  |-> rbase % MOD,
                   need  |-> [o \in Slots |-> ~done[rbase + o]],
                   abase |-> rbase,
                   sd    |-> done]
  <1>0. /\ rbase \in Nat /\ nxt \in Nat /\ open \subseteq Nat
        /\ done \in [Ids -> BOOLEAN]
    BY DEF Inv, TypeOK
  <1>1. p \in Poll BY <1>0, ModRange, IdsNat DEF Poll, Slots
  <1>2. up \in Seq(Poll) /\ Len(up) \in Nat BY SeqTypes DEF Inv, TypeOK
  <1>3. /\ up' \in Seq(Poll)
        /\ Len(up') = Len(up) + 1
        /\ \A i \in 1 .. Len(up) : up'[i] = up[i]
        /\ up'[Len(up) + 1] = p
    BY <1>1, <1>2, AppendProperties DEF SendPoll
  <1>4. rbase \in open \/ rbase >= nxt
    <2>1. ~done[rbase] BY DEF Inv, R2
    <2>2. rbase \notin retired BY <2>1 DEF Inv, Safety, RetireSafe
    <2>3. CASE rbase < nxt BY <2>2, <1>0, <2>3 DEF Inv, S2
    <2> QED BY <2>3, <1>0
  <1>5. \A b \in open : b < rbase + WIN
    <2> SUFFICES ASSUME NEW b \in open PROVE b < rbase + WIN OBVIOUS
    <2>0. b < nxt /\ nxt - b <= WIN BY DEF Inv, S1
    <2>1. CASE rbase >= nxt BY <2>1, <2>0, <1>0, WinAssm
    <2>2. CASE rbase \in open
      <3>1. nxt - rbase <= WIN BY <2>2 DEF Inv, S1
      <3> QED BY <3>1, <2>0, <1>0, WinAssm
    <2> QED BY <1>4, <2>1, <2>2
  <1>6. UNCHANGED <<open, nxt, down, rbase, done, got, retired>> BY DEF SendPoll
  <1>a. TypeOK' BY <1>3, <1>6 DEF Inv, TypeOK
  <1>b. (Safety /\ S1 /\ S2 /\ S3 /\ R1 /\ R2 /\ R3 /\ D1 /\ D2 /\ D3)'
    BY <1>6 DEF Inv, Safety, RetireSafe, Integrity, S1, S2, S3, R1, R2, R3, D1, D2, D3
  <1>c. P1'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                 PROVE  /\ up'[i].base = up'[i].abase % MOD
                        /\ up'[i].abase <= rbase'
                        /\ \A b \in Ids : up'[i].sd[b] => done'[b]
                        /\ \A b \in 0 .. up'[i].abase - 1 : up'[i].sd[b]
                        /\ ~up'[i].sd[up'[i].abase]
                        /\ \A o \in Slots : up'[i].need[o] = ~up'[i].sd[up'[i].abase + o]
      BY DEF P1
    <2>1. CASE i \in 1 .. Len(up) BY <2>1, <1>3, <1>6 DEF Inv, P1
    <2>2. CASE i = Len(up) + 1
      <3>1. up'[i] = p BY <2>2, <1>3
      <3>2. \A b \in 0 .. rbase - 1 : done[b] BY DEF Inv, R1
      <3>3. ~done[rbase] BY DEF Inv, R2
      <3> QED BY <3>1, <3>2, <3>3, <1>0, <1>6, IdsNat DEF Slots
    <2> QED BY <2>1, <2>2, <1>2, <1>3
  <1>d. P2'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up'), NEW j \in 1 .. Len(up'), i < j
                 PROVE  /\ up'[i].abase <= up'[j].abase
                        /\ \A b \in Ids : up'[i].sd[b] => up'[j].sd[b]
      BY DEF P2
    <2>1. CASE j \in 1 .. Len(up)
      <3>1. i \in 1 .. Len(up) /\ up'[i] = up[i] /\ up'[j] = up[j] BY <2>1, <1>2, <1>3
      <3> QED BY <3>1, <2>1 DEF Inv, P2
    <2>2. CASE j = Len(up) + 1
      <3>1. i \in 1 .. Len(up) BY <2>2, <1>2, <1>3
      <3>2. up'[j] = p /\ up'[i] = up[i] BY <2>2, <3>1, <1>3
      <3>3. up[i].abase <= rbase /\ \A x \in Ids : up[i].sd[x] => done[x]
        BY <3>1 DEF Inv, P1
      <3> QED BY <3>2, <3>3
    <2> QED BY <2>1, <2>2, <1>2, <1>3
  <1>e. P3'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                 PROVE  up'[i].abase \in open' \/ up'[i].abase >= nxt'
      BY DEF P3
    <2>1. CASE i \in 1 .. Len(up) BY <2>1, <1>3, <1>6 DEF Inv, P3
    <2>2. CASE i = Len(up) + 1 BY <2>2, <1>3, <1>4, <1>6
    <2> QED BY <2>1, <2>2, <1>2, <1>3
  <1>f. P4'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                 PROVE  \A b \in open' : b < up'[i].abase + WIN
      BY DEF P4
    <2>1. CASE i \in 1 .. Len(up) BY <2>1, <1>3, <1>6 DEF Inv, P4
    <2>2. CASE i = Len(up) + 1 BY <2>2, <1>3, <1>5, <1>6
    <2> QED BY <2>1, <2>2, <1>2, <1>3
  <1> QED BY <1>a, <1>b, <1>c, <1>d, <1>e, <1>f DEF Inv

LEMMA OpenInv == Inv /\ Open => Inv'
  <1> SUFFICES ASSUME Inv, Open PROVE Inv' OBVIOUS
  <1>0. nxt \in Nat /\ rbase \in Nat /\ open \subseteq Nat /\ retired \subseteq Nat
    BY DEF Inv, TypeOK
  <1>1. UNCHANGED <<down, up, rbase, done, got, retired>> BY DEF Open
  <1>2. open' = open \cup {nxt} /\ nxt' = nxt + 1 BY DEF Open
  <1>a. TypeOK' BY <1>0, <1>1, <1>2 DEF Inv, TypeOK
  <1>b. (Safety /\ R1 /\ R2)' BY <1>1 DEF Inv, Safety, RetireSafe, Integrity, R1, R2
  <1>c. S1'
    <2> SUFFICES ASSUME NEW b \in open' PROVE b < nxt' /\ nxt' - b <= WIN
      BY DEF S1
    <2>1. CASE b = nxt BY <2>1, <1>0, <1>2, WinAssm
    <2>2. CASE b \in open
      <3>1. nxt - b < WIN BY <2>2 DEF Open
      <3>2. b < nxt BY <2>2 DEF Inv, S1
      <3> QED BY <3>1, <3>2, <2>2, <1>0, <1>2, WinAssm
    <2> QED BY <2>1, <2>2, <1>2
  <1>d1. S2'
    <2> SUFFICES ASSUME NEW b \in 0 .. nxt' - 1 PROVE b \in open' \/ b \in retired'
      BY DEF S2
    <2>1. CASE b = nxt BY <2>1, <1>2
    <2>2. CASE b # nxt
      <3>1. b \in 0 .. nxt - 1 BY <2>2, <1>0, <1>2
      <3> QED BY <3>1, <1>1, <1>2 DEF Inv, S2
    <2> QED BY <2>1, <2>2
  <1>d2. S3' BY <1>0, <1>1, <1>2 DEF Inv, S3
  <1>d3. R3' BY <1>0, <1>1, <1>2, IdsNat DEF Inv, R3
  <1>e0. TypeOK BY DEF Inv
  <1>e1. D1'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(down')
                 PROVE  /\ down'[i].wire = down'[i].true % MOD
                        /\ down'[i].true < down'[i].snxt /\ down'[i].snxt <= nxt'
                        /\ down'[i].snxt <= down'[i].true + WIN
                        /\ down'[i].true < rbase' + WIN
      BY DEF D1
    <2>1. i \in 1 .. Len(down) /\ down'[i] = down[i] /\ down[i] \in Data BY <1>1, <1>e0, SeqTypes
    <2>2. down[i].snxt <= nxt BY <2>1 DEF Inv, D1
    <2> QED BY <2>1, <2>2, <1>0, <1>1, <1>2 DEF Inv, D1, Data
  <1>e2. (D2 /\ D3)' BY <1>0, <1>1, <1>2 DEF Inv, D2, D3
  <1>f. (P1 /\ P2)' BY <1>1 DEF Inv, P1, P2
  <1>g. P3' BY <1>0, <1>1, <1>2 DEF Inv, P3, TypeOK, Poll
  <1>h. P4'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up'), NEW b \in open'
                 PROVE  b < up'[i].abase + WIN
      BY DEF P4
    <2>0. i \in 1 .. Len(up) /\ up'[i] = up[i] BY <1>1
    <2>9. up[i].abase \in Nat BY <2>0 DEF Inv, TypeOK, Poll
    <2>1. CASE b \in open BY <2>1, <2>0 DEF Inv, P4
    <2>2. CASE b = nxt
      <3>1. CASE up[i].abase \in open
        <4>1. nxt - up[i].abase < WIN BY <3>1 DEF Open
        <4> QED BY <4>1, <2>2, <2>0, <2>9, <1>0, WinAssm
      <3>2. CASE up[i].abase >= nxt BY <3>2, <2>2, <2>0, <2>9, <1>0, WinAssm
      <3> QED BY <3>1, <3>2, <2>0 DEF Inv, P3
    <2> QED BY <2>1, <2>2, <1>2
  <1> QED BY <1>a, <1>b, <1>c, <1>d1, <1>d2, <1>d3, <1>e1, <1>e2, <1>f, <1>g, <1>h DEF Inv
(* ---- the sender takes a poll: retirement ---- *)
LEMMA RecvUpInv == Inv /\ RecvUp => Inv'
  <1> SUFFICES ASSUME Inv, RecvUp PROVE Inv' OBVIOUS
  <1> DEFINE m == Head(up)
  <1>0. /\ TypeOK /\ nxt \in Nat /\ rbase \in Nat /\ open \subseteq Nat
        /\ retired \subseteq Nat /\ up \in Seq(Poll) /\ up # << >>
    BY DEF Inv, TypeOK, RecvUp
  <1>1. Len(up) \in Nat /\ Len(up) >= 1 /\ m = up[1] /\ m \in Poll
    BY <1>0, SeqTypes, HeadTailProperties, LenProperties, EmptySeq
  <1>2. /\ up' = Tail(up) /\ up' \in Seq(Poll) /\ Len(up') = Len(up) - 1
        /\ \A i \in 1 .. Len(up') : up'[i] = up[i + 1]
    BY <1>0, HeadTailProperties DEF RecvUp
  <1>3. UNCHANGED <<nxt, down, rbase, done, got>> BY DEF RecvUp
  <1>4. /\ open' = {b \in open : Keep(b, m)}
        /\ retired' = retired \cup {b \in open : ~Keep(b, m)}
    BY DEF RecvUp
  <1>5. m.abase \in Nat /\ m.base = m.abase % MOD
        /\ \A b \in open : b < m.abase + WIN
        /\ \A b \in Ids : m.sd[b] => done[b]
        /\ \A b \in 0 .. m.abase - 1 : m.sd[b]
        /\ \A o \in Slots : m.need[o] = ~m.sd[m.abase + o]
    BY <1>1 DEF Inv, P1, P4, Poll
  <1>5t. m.sd \in [Nat -> BOOLEAN] /\ m.need \in [Slots -> BOOLEAN] BY <1>1, IdsNat DEF Poll
  <1>5f. \A o \in Slots : m.need[o] = ~m.sd[m.abase + o] BY <1>1 DEF Inv, P1
  <1>5g. \A x \in 0 .. m.abase - 1 : m.sd[x] BY <1>1 DEF Inv, P1
  \* what Keep says of an open block at or above the poll's base
  <1>6. \A b \in open : b >= m.abase => (Keep(b, m) <=> ~m.sd[b])
    <2> SUFFICES ASSUME NEW b \in open, b >= m.abase
                 PROVE  Keep(b, m) <=> ~m.sd[b]
      OBVIOUS
    <2>0. b >= m.abase OBVIOUS
    <2>1. b \in Nat /\ b < m.abase + WIN BY <1>0, <1>5
    <2>2. ((b % MOD) + MOD - (m.abase % MOD)) % MOD = b - m.abase
      BY <2>0, <2>1, <1>5, ResolveIn
    <2>3. b - m.abase \in Slots BY <2>0, <2>1, <1>5, WinAssm DEF Slots
    <2>4. Keep(b, m) <=> (b - m.abase < WIN /\ m.need[b - m.abase]) BY <2>2, <1>5 DEF Keep
    <2>5a. m.need[b - m.abase] = ~m.sd[m.abase + (b - m.abase)] BY <2>3, <1>5f
    <2>5b. m.abase + (b - m.abase) = b BY <2>1, <1>5
    <2>5. m.need[b - m.abase] = ~m.sd[b] BY <2>5a, <2>5b
    <2> QED BY <2>3, <2>4, <2>5, <1>5t, <2>1, WinAssm DEF Slots
  \* a block the poll retires was decoded when it was sent
  <1>7. \A b \in open : ~Keep(b, m) => done[b]
    <2> SUFFICES ASSUME NEW b \in open, ~Keep(b, m) PROVE done[b] OBVIOUS
    <2>0. b \in Nat /\ ~Keep(b, m) BY <1>0
    <2>1. b < m.abase => done[b]
      <3> SUFFICES ASSUME b < m.abase PROVE done[b] OBVIOUS
      <3>0. b < m.abase OBVIOUS
      <3>1. b \in 0 .. m.abase - 1 BY <3>0, <2>0, <1>5
      <3>2. m.sd[b] BY <3>1, <1>5g
      <3> QED BY <3>2, <2>0, <1>5, IdsNat
    <2>2. b >= m.abase => done[b]
      <3> SUFFICES ASSUME b >= m.abase PROVE done[b] OBVIOUS
      <3>0. b >= m.abase OBVIOUS
      <3>1. ~m.sd[b] => Keep(b, m) BY <3>0, <1>6
      <3>2. m.sd[b] BY <3>1, <2>0, <1>5t
      <3> QED BY <3>2, <2>0, <1>5, IdsNat
    <2> QED BY <2>1, <2>2, <2>0, <1>5
  <1>a. TypeOK' BY <1>0, <1>2, <1>3, <1>4 DEF TypeOK
  <1>b. RetireSafe' BY <1>3, <1>4, <1>7 DEF Inv, Safety, RetireSafe
  <1>c. Integrity' BY <1>3 DEF Inv, Safety, Integrity
  <1>d. S1' BY <1>3, <1>4 DEF Inv, S1
  <1>e. S2'
    <2> SUFFICES ASSUME NEW b \in 0 .. nxt' - 1 PROVE b \in open' \/ b \in retired'
      BY DEF S2
    <2>1. b \in open \/ b \in retired BY <1>3 DEF Inv, S2
    <2> QED BY <2>1, <1>4
  <1>f. S3' BY <1>3, <1>4 DEF Inv, S1, S3
  <1>g. (R1 /\ R2 /\ R3 /\ D1 /\ D2 /\ D3)' BY <1>3 DEF Inv, R1, R2, R3, D1, D2, D3
  <1>h. P1'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                 PROVE  /\ up'[i].base = up'[i].abase % MOD
                        /\ up'[i].abase <= rbase'
                        /\ \A b \in Ids : up'[i].sd[b] => done'[b]
                        /\ \A b \in 0 .. up'[i].abase - 1 : up'[i].sd[b]
                        /\ ~up'[i].sd[up'[i].abase]
                        /\ \A o \in Slots : up'[i].need[o] = ~up'[i].sd[up'[i].abase + o]
      BY DEF P1
    <2>1. i + 1 \in 1 .. Len(up) /\ up'[i] = up[i + 1] BY <1>1, <1>2
    <2> QED BY <2>1, <1>3 DEF Inv, P1
  <1>i. P2'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up'), NEW j \in 1 .. Len(up'), i < j
                 PROVE  /\ up'[i].abase <= up'[j].abase
                        /\ \A b \in Ids : up'[i].sd[b] => up'[j].sd[b]
      BY DEF P2
    <2>1. i + 1 \in 1 .. Len(up) /\ j + 1 \in 1 .. Len(up) /\ i + 1 < j + 1
          /\ up'[i] = up[i + 1] /\ up'[j] = up[j + 1]
      BY <1>1, <1>2
    <2> QED BY <2>1 DEF Inv, P2
  \* a later poll's base stays open: this older poll saw it undecoded
  <1>j. P3'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                 PROVE  up'[i].abase \in open' \/ up'[i].abase >= nxt'
      BY DEF P3
    <2>1. i + 1 \in 1 .. Len(up) /\ up'[i] = up[i + 1] /\ 1 < i + 1 BY <1>1, <1>2
    <2> DEFINE a == up[i + 1].abase
    <2>2. a \in open \/ a >= nxt BY <2>1 DEF Inv, P3
    <2>3. CASE a \in open
      <3>1. m.abase <= a /\ \A x \in Ids : m.sd[x] => up[i + 1].sd[x]
        BY <2>1, <1>1 DEF Inv, P2
      <3>2. ~up[i + 1].sd[a] BY <2>1 DEF Inv, P1
      <3>3. a \in Nat BY <2>3, <1>0
      <3>4. ~m.sd[a] BY <3>1, <3>2, <3>3, IdsNat
      <3>5. Keep(a, m) BY <2>3, <3>1, <3>4, <1>6
      <3> QED BY <2>1, <2>3, <3>5, <1>4
    <2>4. CASE a >= nxt BY <2>1, <2>4, <1>3
    <2> QED BY <2>2, <2>3, <2>4
  <1>k. P4'
    <2> SUFFICES ASSUME NEW i \in 1 .. Len(up'), NEW b \in open'
                 PROVE  b < up'[i].abase + WIN
      BY DEF P4
    <2>1. i + 1 \in 1 .. Len(up) /\ up'[i] = up[i + 1] /\ b \in open BY <1>1, <1>2, <1>4
    <2> QED BY <2>1 DEF Inv, P4
  <1> QED BY <1>a, <1>b, <1>c, <1>d, <1>e, <1>f, <1>g, <1>h, <1>i, <1>j, <1>k
          DEF Inv, Safety
(* ---- the receiver takes a frame: resolution, decoding, delivery ---- *)
LEMMA RecvDownInv == Inv /\ RecvDown => Inv'
  <1> SUFFICES ASSUME Inv, RecvDown PROVE Inv' OBVIOUS
  <1> DEFINE m == Head(down)
  <1>0. /\ TypeOK /\ nxt \in Nat /\ rbase \in Nat /\ open \subseteq Nat
        /\ down \in Seq(Data) /\ down # << >> /\ done \in [Nat -> BOOLEAN]
        /\ got \in [Nat -> SUBSET Nat]
    BY IdsNat DEF Inv, TypeOK, RecvDown
  <1>1. Len(down) \in Nat /\ Len(down) >= 1 /\ m = down[1] /\ m \in Data
    BY <1>0, SeqTypes, HeadTailProperties, LenProperties, EmptySeq
  <1>2. /\ down' = Tail(down) /\ down' \in Seq(Data) /\ Len(down') = Len(down) - 1
        /\ \A i \in 1 .. Len(down') : down'[i] = down[i + 1]
    BY <1>0, HeadTailProperties DEF RecvDown
  <1>3. UNCHANGED <<open, nxt, up, retired>> BY DEF RecvDown
  \* the frame's block lies within WIN of the base, either side
  <1>4. /\ m.wire = m.true % MOD /\ m.true \in Nat /\ m.snxt \in Nat
        /\ m.true < m.snxt /\ m.snxt <= nxt /\ m.snxt <= m.true + WIN
        /\ m.true < rbase + WIN
    BY <1>1 DEF Inv, D1, Data
  <1>5. rbase <= m.snxt
    <2>1. rbase = 0 => rbase <= m.snxt BY <1>4, <1>0
    <2>2. rbase > 0 => rbase <= m.snxt
      <3> SUFFICES ASSUME rbase > 0 PROVE rbase <= m.snxt OBVIOUS
      <3>1. rbase - 1 \in 0 .. rbase - 1 BY <1>0
      <3>2. done[rbase - 1] BY <3>1 DEF Inv, R1
      <3>3. rbase - 1 < m.snxt BY <3>2, <1>1, <1>0, IdsNat DEF Inv, D3
      <3> QED BY <3>3, <1>4, <1>0
    <2> QED BY <2>1, <2>2, <1>0
  <1>6. m.true + WIN >= rbase BY <1>4, <1>5, <1>0, WinAssm
  \* the resolved slot is the frame's own block, when it is in the window
  <1>7. InWin(m) => Abs(m) = m.true
    <2> SUFFICES ASSUME InWin(m) PROVE Abs(m) = m.true OBVIOUS
    <2>0. InWin(m) OBVIOUS
    <2>1. m.true >= rbase
      <3> SUFFICES ASSUME m.true < rbase PROVE FALSE BY <1>4, <1>0
      <3>1. m.true < rbase OBVIOUS
      <3>2. ((m.true % MOD) + MOD - (rbase % MOD)) % MOD >= WIN
        BY <3>1, <1>6, <1>4, <1>0, ResolveBehind
      <3>3. (m.wire + MOD - (rbase % MOD)) % MOD = ((m.true % MOD) + MOD - (rbase % MOD)) % MOD
        BY <1>4
      <3>4. (m.wire + MOD - (rbase % MOD)) % MOD < WIN BY <2>0 DEF InWin
      <3> DEFINE X == (m.wire + MOD - (rbase % MOD)) % MOD
                 Y == ((m.true % MOD) + MOD - (rbase % MOD)) % MOD
      <3>5. X \in Nat /\ Y \in Nat
        BY <1>4, <1>0, Win8 DEF MOD, Data
      <3>6. X < WIN /\ X = Y /\ Y >= WIN BY <3>2, <3>3, <3>4
      <3> HIDE DEF X, Y
      <3> QED BY <3>5, <3>6, WinAssm
    <2>2. ((m.true % MOD) + MOD - (rbase % MOD)) % MOD = m.true - rbase
      BY <2>1, <1>4, <1>0, ResolveIn
    <2> QED BY <2>2, <1>4, <1>0 DEF Abs
  <1>8. CASE ~(InWin(m) /\ ~done[Abs(m)])
    <2>1. UNCHANGED <<got, done, rbase>> BY <1>8 DEF RecvDown
    <2>a. TypeOK' BY <1>0, <1>2, <1>3, <2>1 DEF TypeOK
    <2>b. (Safety /\ S1 /\ S2 /\ S3 /\ R1 /\ R2 /\ R3)'
      BY <1>3, <2>1 DEF Inv, Safety, RetireSafe, Integrity, S1, S2, S3, R1, R2, R3
    <2>c. (P1 /\ P2 /\ P3 /\ P4)' BY <1>3, <2>1 DEF Inv, P1, P2, P3, P4
    <2>d. D1'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down')
                   PROVE  /\ down'[i].wire = down'[i].true % MOD
                          /\ down'[i].true < down'[i].snxt /\ down'[i].snxt <= nxt'
                          /\ down'[i].snxt <= down'[i].true + WIN
                          /\ down'[i].true < rbase' + WIN
        BY DEF D1
      <3>1. i + 1 \in 1 .. Len(down) /\ down'[i] = down[i + 1] BY <1>1, <1>2
      <3> QED BY <3>1, <1>3, <2>1 DEF Inv, D1
    <2>e. D2'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down'), NEW j \in 1 .. Len(down'), i < j
                   PROVE  down'[i].snxt <= down'[j].snxt
        BY DEF D2
      <3>1. i + 1 \in 1 .. Len(down) /\ j + 1 \in 1 .. Len(down) /\ i + 1 < j + 1
            /\ down'[i] = down[i + 1] /\ down'[j] = down[j + 1]
        BY <1>1, <1>2
      <3> QED BY <3>1 DEF Inv, D2
    <2>f. D3'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down'), NEW b \in Ids, done'[b]
                   PROVE  b < down'[i].snxt
        BY DEF D3
      <3>1. i + 1 \in 1 .. Len(down) /\ down'[i] = down[i + 1] BY <1>1, <1>2
      <3> QED BY <3>1, <2>1 DEF Inv, D3
    <2> QED BY <2>a, <2>b, <2>c, <2>d, <2>e, <2>f DEF Inv, Safety
  <1>9. CASE InWin(m) /\ ~done[Abs(m)]
    <2>0. Abs(m) = m.true /\ ~done[m.true] BY <1>9, <1>7
    <2>1. PICK dec \in BOOLEAN, x \in Ids :
            /\ got' = [got EXCEPT ![Abs(m)] = @ \cup {m.true}]
            /\ done' = [done EXCEPT ![Abs(m)] = dec]
            /\ x >= rbase /\ ~done'[x] /\ \A y \in rbase .. x - 1 : done'[y]
            /\ rbase' = x
      BY <1>9 DEF RecvDown
    <2>2. /\ got' = [got EXCEPT ![m.true] = @ \cup {m.true}]
          /\ done' = [done EXCEPT ![m.true] = dec]
          /\ rbase' >= rbase /\ rbase' \in Nat
          /\ ~done'[rbase'] /\ \A y \in rbase .. rbase' - 1 : done'[y]
      BY <2>0, <2>1, IdsNat
    \* done only grows, and only by the frame's own block, which is below its snxt
    <2>3. \A b \in Nat : done[b] => done'[b]
      <3> SUFFICES ASSUME NEW b \in Nat, done[b] PROVE done'[b] OBVIOUS
      <3>1. b # m.true BY <2>0
      <3> QED BY <3>1, <2>2, <1>0
    <2>4. \A b \in Nat : done'[b] => done[b] \/ b = m.true BY <2>2, <1>0, <1>4
    <2>a. TypeOK' BY <1>0, <1>2, <1>3, <2>2, <1>4, IdsNat DEF TypeOK
    <2>b. RetireSafe' BY <1>3, <2>3, <1>0, IdsNat DEF Inv, Safety, RetireSafe, TypeOK
    <2>c. Integrity'
      <3> SUFFICES ASSUME NEW b \in Ids PROVE got'[b] \subseteq {b} BY DEF Integrity
      <3>1. got[b] \subseteq {b} BY DEF Inv, Safety, Integrity
      <3> QED BY <3>1, <2>2, <1>0, <1>4, IdsNat
    <2>d. (S1 /\ S2 /\ S3)' BY <1>3 DEF Inv, S1, S2, S3
    <2>e. R1'
      <3> SUFFICES ASSUME NEW b \in 0 .. rbase' - 1 PROVE done'[b] BY DEF R1
      <3>1. b < rbase => done[b] BY <1>0 DEF Inv, R1
      <3> QED BY <3>1, <2>2, <2>3, <1>0, IdsNat
    <2>f. R2' BY <2>2 DEF R2
    <2>g. R3'
      <3> SUFFICES ASSUME NEW b \in Ids, done'[b] PROVE b < nxt' BY DEF R3
      <3>1. done[b] \/ b = m.true BY <2>4, IdsNat
      <3>2. done[b] => b < nxt BY DEF Inv, R3
      <3> QED BY <3>1, <3>2, <1>3, <1>4, <1>0
    <2>h. D1'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down')
                   PROVE  /\ down'[i].wire = down'[i].true % MOD
                          /\ down'[i].true < down'[i].snxt /\ down'[i].snxt <= nxt'
                          /\ down'[i].snxt <= down'[i].true + WIN
                          /\ down'[i].true < rbase' + WIN
        BY DEF D1
      <3>1. i + 1 \in 1 .. Len(down) /\ down'[i] = down[i + 1] /\ down[i + 1] \in Data
        BY <1>1, <1>2, <1>0, SeqTypes
      <3>2. /\ down[i + 1].wire = down[i + 1].true % MOD
            /\ down[i + 1].true < down[i + 1].snxt /\ down[i + 1].snxt <= nxt
            /\ down[i + 1].snxt <= down[i + 1].true + WIN
            /\ down[i + 1].true < rbase + WIN
        BY <3>1 DEF Inv, D1
      <3>3. down[i + 1].true \in Nat BY <3>1 DEF Data
      <3> QED BY <3>1, <3>2, <3>3, <1>3, <2>2, <1>0, WinAssm
    <2>i. D2'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down'), NEW j \in 1 .. Len(down'), i < j
                   PROVE  down'[i].snxt <= down'[j].snxt
        BY DEF D2
      <3>1. i + 1 \in 1 .. Len(down) /\ j + 1 \in 1 .. Len(down) /\ i + 1 < j + 1
            /\ down'[i] = down[i + 1] /\ down'[j] = down[j + 1]
        BY <1>1, <1>2
      <3> QED BY <3>1 DEF Inv, D2
    \* the newly decoded block is below every later frame's snxt (FIFO order)
    <2>j. D3'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(down'), NEW b \in Ids, done'[b]
                   PROVE  b < down'[i].snxt
        BY DEF D3
      <3>1. i + 1 \in 1 .. Len(down) /\ down'[i] = down[i + 1] /\ 1 < i + 1 BY <1>1, <1>2
      <3>2. m.snxt <= down[i + 1].snxt BY <3>1, <1>1 DEF Inv, D2
      <3>3. done[b] \/ b = m.true BY <2>4, IdsNat
      <3>4. done[b] => b < down[i + 1].snxt BY <3>1 DEF Inv, D3
      <3> QED BY <3>1, <3>2, <3>3, <3>4, <1>4, <1>0, SeqTypes DEF Data
    <2>k. P1'
      <3> SUFFICES ASSUME NEW i \in 1 .. Len(up')
                   PROVE  /\ up'[i].base = up'[i].abase % MOD
                          /\ up'[i].abase <= rbase'
                          /\ \A b \in Ids : up'[i].sd[b] => done'[b]
                          /\ \A b \in 0 .. up'[i].abase - 1 : up'[i].sd[b]
                          /\ ~up'[i].sd[up'[i].abase]
                          /\ \A o \in Slots : up'[i].need[o] = ~up'[i].sd[up'[i].abase + o]
        BY DEF P1
      <3>1. up'[i] = up[i] /\ i \in 1 .. Len(up) BY <1>3
      <3>2. /\ up[i].base = up[i].abase % MOD
            /\ up[i].abase <= rbase
            /\ \A b \in Ids : up[i].sd[b] => done[b]
            /\ \A b \in 0 .. up[i].abase - 1 : up[i].sd[b]
            /\ ~up[i].sd[up[i].abase]
            /\ \A o \in Slots : up[i].need[o] = ~up[i].sd[up[i].abase + o]
        BY <3>1 DEF Inv, P1
      <3>3. up[i].abase \in Nat BY <3>1, <1>0, SeqTypes DEF Poll, TypeOK
      <3>4. \A b \in Ids : up[i].sd[b] => done'[b] BY <3>2, <2>3, IdsNat
      <3> QED BY <3>1, <3>2, <3>3, <3>4, <2>2, <1>0
    <2>l. (P2 /\ P3 /\ P4)' BY <1>3 DEF Inv, P2, P3, P4
    <2> QED BY <2>a, <2>b, <2>c, <2>d, <2>e, <2>f, <2>g, <2>h, <2>i, <2>j, <2>k, <2>l
            DEF Inv, Safety
  <1> QED BY <1>8, <1>9

(* ---- the theorem ---- *)
THEOREM Invariant == Spec => []Inv
  <1>1. Init => Inv BY InitInv
  <1>2. Inv /\ [Next]_vars => Inv'
    <2> SUFFICES ASSUME Inv, [Next]_vars PROVE Inv' OBVIOUS
    <2>1. CASE Open BY <2>1, OpenInv
    <2>2. CASE \E b \in open : SendPiece(b) BY <2>2, SendPieceInv
    <2>3. CASE RecvUp BY <2>3, RecvUpInv
    <2>4. CASE SendPoll BY <2>4, SendPollInv
    <2>5. CASE RecvDown BY <2>5, RecvDownInv
    <2>6. CASE LoseDown BY <2>6, LoseDownInv
    <2>7. CASE LoseUp BY <2>7, LoseUpInv
    <2>8. CASE UNCHANGED vars
      BY <2>8 DEF vars, Inv, TypeOK, Safety, RetireSafe, Integrity,
         S1, S2, S3, R1, R2, R3, D1, D2, D3, P1, P2, P3, P4
    <2> QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8 DEF Next
  <1> QED BY <1>1, <1>2, PTL DEF Spec

THEOREM SafetyHolds == Spec => []Safety
  BY Invariant, PTL DEF Inv
=============================================================================
