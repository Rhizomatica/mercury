--------------------------- MODULE TraceHandshake ----------------------------
(***************************************************************************)
(* Trace validation of ConnectHandshake: is a run of the code one of the  *)
(* spec's behaviours?  tests/sim/connect_trace runs the two real FSMs     *)
(* (tests/sim) and writes, as module TraceData, every change of what the  *)
(* spec sees of them: both ends' states, session ids, nonces and start    *)
(* rungs (-1: not read).  The rest of the spec's state -- CALLs left, the *)
(* ACCEPTs in the air, bandwidths, encryption -- is left to TLC to find.  *)
(*                                                                         *)
(* Each step of the spec either moves to the trace's next row or keeps    *)
(* the row it is on (a step the trace cannot see: a CALL lost, an ACCEPT  *)
(* keyed again).  The trace is a behaviour of the spec iff TLC reaches    *)
(* its last row: TraceNotDone, violated, says it did.  trace_check.sh     *)
(* runs both.                                                             *)
(***************************************************************************)
EXTENDS ConnectHandshake, TraceData, Sequences, TLC

VARIABLE i      \* the row of Trace the spec's state matches

\* The state matches row r, primed: the next state does.
Obs(r) == /\ cSt = r.cSt /\ cSid = r.cSid /\ eSt = r.eSt
          /\ (r.cNonce = -1 \/ cNonce = r.cNonce)
          /\ (r.eSid = -1 \/ eSid = r.eSid)
          /\ (r.eNonce = -1 \/ eNonce = r.eNonce)
          /\ (r.cLevel = -1 \/ cLevel = r.cLevel)
          /\ (r.eLevel = -1 \/ eLevel = r.eLevel)
ObsP(r) == /\ cSt' = r.cSt /\ cSid' = r.cSid /\ eSt' = r.eSt
           /\ (r.cNonce = -1 \/ cNonce' = r.cNonce)
           /\ (r.eSid = -1 \/ eSid' = r.eSid)
           /\ (r.eNonce = -1 \/ eNonce' = r.eNonce)
           /\ (r.cLevel = -1 \/ cLevel' = r.cLevel)
           /\ (r.eLevel = -1 \/ eLevel' = r.eLevel)

\* TLC register 1: the furthest row matched (trace_check.sh prints it when
\* the trace is rejected: the row after it is where the spec and the code
\* part).  Run with one worker.
Furthest == IF i' > TLCGet(1) THEN TLCSet(1, i') ELSE TRUE

TInit == Init /\ i = 1 /\ Obs(Trace[1]) /\ TLCSet(1, 1)
TNext == /\ Next
         /\ \/ /\ i < Len(Trace) /\ i' = i + 1
               /\ ObsP(Trace[i + 1])
            \/ /\ i' = i
               /\ ObsP(Trace[i])
         /\ Furthest
TSpec == TInit /\ [][TNext]_<<vars, i>>

\* Violated once the whole trace has been matched.
TraceNotDone == i < Len(Trace)
TracePost == PrintT(<<"furthest row", TLCGet(1), "of", Len(Trace)>>)
=============================================================================
