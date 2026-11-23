---------------------------- MODULE TraceTeardown -----------------------------
(***************************************************************************)
(* Trace validation of SessionTeardown: is a run of the code one of the   *)
(* spec's behaviours?  tests/sim/teardown_trace runs the two real FSMs    *)
(* (tests/sim) through a connect, some data and a hang-up, and writes, as *)
(* module TraceData, every change of what the spec sees of them: each     *)
(* end's state, session id, whether its DISCONNECT is out (while ending)  *)
(* and whether the caller holds its DISCONNECT for the drain (while       *)
(* connected); -1: not read.  The rest -- the frames on their way, the    *)
(* last words, the host's notices -- is left to TLC to find.              *)
(*                                                                         *)
(* As TraceHandshake: each step of the spec moves to the trace's next row *)
(* or keeps the row it is on, and the trace is a behaviour of the spec    *)
(* iff TLC reaches its last row (TraceNotDone violated).                  *)
(***************************************************************************)
EXTENDS SessionTeardown, TraceData, TLC

VARIABLE i

B(x) == x = 1     \* a 0/1 field of the trace

Obs(r) == /\ \A e \in E : /\ st[e] = r.st[e] /\ sid[e] = r.sid[e]
                          /\ (r.sent[e] = -1 \/ sent[e] = B(r.sent[e]))
          /\ (r.mdrain = -1 \/ mdrain = B(r.mdrain))
ObsP(r) == /\ \A e \in E : /\ st'[e] = r.st[e] /\ sid'[e] = r.sid[e]
                           /\ (r.sent[e] = -1 \/ sent'[e] = B(r.sent[e]))
           /\ (r.mdrain = -1 \/ mdrain' = B(r.mdrain))

Furthest == IF i' > TLCGet(1) THEN TLCSet(1, i') ELSE TRUE

TInit == Init /\ i = 1 /\ Obs(Trace[1]) /\ TLCSet(1, 1)
TNext == /\ Next
         /\ \/ /\ i < Len(Trace) /\ i' = i + 1
               /\ ObsP(Trace[i + 1])
            \/ /\ i' = i
               /\ ObsP(Trace[i])
         /\ Furthest

TraceNotDone == i < Len(Trace)
TracePost == PrintT(<<"furthest row", TLCGet(1), "of", Len(Trace)>>)
=============================================================================
