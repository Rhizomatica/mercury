---- MODULE ProofCheck ----
\* TLC harness for CarouselProof: bounded ids, and every reachable state must
\* satisfy the inductive invariant the proof uses.
EXTENDS CarouselProof
Bounded == Len(down) <= 3 /\ Len(up) <= 3
====
