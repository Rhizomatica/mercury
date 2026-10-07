---- MODULE CarouselTurns_TTrace_1791319363 ----
EXTENDS Sequences, TLCExt, Toolbox, CarouselTurns, Naturals, TLC

_expression ==
    LET CarouselTurns_TEExpression == INSTANCE CarouselTurns_TEExpression
    IN CarouselTurns_TEExpression!expression
----

_trace ==
    LET CarouselTurns_TETrace == INSTANCE CarouselTurns_TETrace
    IN CarouselTurns_TETrace!trace
----

_inv ==
    ~(
        TLCGet("level") = Len(_TETrace)
        /\
        sOn = (TRUE)
        /\
        t = (8)
        /\
        mLog = ({})
        /\
        mR = (1)
        /\
        anchor = ([e |-> 0, eh |-> 0, r |-> 0, i |-> -1])
        /\
        sSlot = ([e |-> 0, i |-> -1])
        /\
        mEnd = (9)
        /\
        sEnd = (9)
        /\
        mOn = (TRUE)
        /\
        inflight = ({})
        /\
        mDone = ({})
    )
----

_init ==
    /\ sOn = _TETrace[1].sOn
    /\ mLog = _TETrace[1].mLog
    /\ inflight = _TETrace[1].inflight
    /\ mR = _TETrace[1].mR
    /\ sSlot = _TETrace[1].sSlot
    /\ sEnd = _TETrace[1].sEnd
    /\ t = _TETrace[1].t
    /\ mDone = _TETrace[1].mDone
    /\ mEnd = _TETrace[1].mEnd
    /\ mOn = _TETrace[1].mOn
    /\ anchor = _TETrace[1].anchor
----

_next ==
    /\ \E i,j \in DOMAIN _TETrace:
        /\ \/ /\ j = i + 1
              /\ i = TLCGet("level")
        /\ sOn  = _TETrace[i].sOn
        /\ sOn' = _TETrace[j].sOn
        /\ mLog  = _TETrace[i].mLog
        /\ mLog' = _TETrace[j].mLog
        /\ inflight  = _TETrace[i].inflight
        /\ inflight' = _TETrace[j].inflight
        /\ mR  = _TETrace[i].mR
        /\ mR' = _TETrace[j].mR
        /\ sSlot  = _TETrace[i].sSlot
        /\ sSlot' = _TETrace[j].sSlot
        /\ sEnd  = _TETrace[i].sEnd
        /\ sEnd' = _TETrace[j].sEnd
        /\ t  = _TETrace[i].t
        /\ t' = _TETrace[j].t
        /\ mDone  = _TETrace[i].mDone
        /\ mDone' = _TETrace[j].mDone
        /\ mEnd  = _TETrace[i].mEnd
        /\ mEnd' = _TETrace[j].mEnd
        /\ mOn  = _TETrace[i].mOn
        /\ mOn' = _TETrace[j].mOn
        /\ anchor  = _TETrace[i].anchor
        /\ anchor' = _TETrace[j].anchor

\* Uncomment the ASSUME below to write the states of the error trace
\* to the given file in Json format. Note that you can pass any tuple
\* to `JsonSerialize`. For example, a sub-sequence of _TETrace.
    \* ASSUME
    \*     LET J == INSTANCE Json
    \*         IN J!JsonSerialize("CarouselTurns_TTrace_1791319363.json", _TETrace)

=============================================================================

 Note that you can extract this module `CarouselTurns_TEExpression`
  to a dedicated file to reuse `expression` (the module in the 
  dedicated `CarouselTurns_TEExpression.tla` file takes precedence 
  over the module `CarouselTurns_TEExpression` below).

---- MODULE CarouselTurns_TEExpression ----
EXTENDS Sequences, TLCExt, Toolbox, CarouselTurns, Naturals, TLC

expression == 
    [
        \* To hide variables of the `CarouselTurns` spec from the error trace,
        \* remove the variables below.  The trace will be written in the order
        \* of the fields of this record.
        sOn |-> sOn
        ,mLog |-> mLog
        ,inflight |-> inflight
        ,mR |-> mR
        ,sSlot |-> sSlot
        ,sEnd |-> sEnd
        ,t |-> t
        ,mDone |-> mDone
        ,mEnd |-> mEnd
        ,mOn |-> mOn
        ,anchor |-> anchor
        
        \* Put additional constant-, state-, and action-level expressions here:
        \* ,_stateNumber |-> _TEPosition
        \* ,_sOnUnchanged |-> sOn = sOn'
        
        \* Format the `sOn` variable as Json value.
        \* ,_sOnJson |->
        \*     LET J == INSTANCE Json
        \*     IN J!ToJson(sOn)
        
        \* Lastly, you may build expressions over arbitrary sets of states by
        \* leveraging the _TETrace operator.  For example, this is how to
        \* count the number of times a spec variable changed up to the current
        \* state in the trace.
        \* ,_sOnModCount |->
        \*     LET F[s \in DOMAIN _TETrace] ==
        \*         IF s = 1 THEN 0
        \*         ELSE IF _TETrace[s].sOn # _TETrace[s-1].sOn
        \*             THEN 1 + F[s-1] ELSE F[s-1]
        \*     IN F[_TEPosition - 1]
    ]

=============================================================================



Parsing and semantic processing can take forever if the trace below is long.
 In this case, it is advised to uncomment the module below to deserialize the
 trace from a generated binary file.

\*
\*---- MODULE CarouselTurns_TETrace ----
\*EXTENDS IOUtils, CarouselTurns, TLC
\*
\*trace == IODeserialize("CarouselTurns_TTrace_1791319363.bin", TRUE)
\*
\*=============================================================================
\*

---- MODULE CarouselTurns_TETrace ----
EXTENDS CarouselTurns, TLC

trace == 
    <<
    ([sOn |-> FALSE,t |-> 0,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 1,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 2,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 3,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 4,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 5,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 6,mLog |-> {},mR |-> 0,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 0,sEnd |-> 0,mOn |-> FALSE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 6,mLog |-> {},mR |-> 1,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 9,sEnd |-> 0,mOn |-> TRUE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 7,mLog |-> {},mR |-> 1,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 9,sEnd |-> 0,mOn |-> TRUE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> FALSE,t |-> 8,mLog |-> {},mR |-> 1,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 9,sEnd |-> 0,mOn |-> TRUE,inflight |-> {},mDone |-> {}]),
    ([sOn |-> TRUE,t |-> 8,mLog |-> {},mR |-> 1,anchor |-> [e |-> 0, eh |-> 0, r |-> 0, i |-> -1],sSlot |-> [e |-> 0, i |-> -1],mEnd |-> 9,sEnd |-> 9,mOn |-> TRUE,inflight |-> {},mDone |-> {}])
    >>
----


=============================================================================

---- CONFIG CarouselTurns_TTrace_1791319363 ----
CONSTANTS
    Tg = 1
    D = 2
    G = 1
    K = 2
    A = 5
    ML = { 1 , 3 }
    RL = { 1 , 3 }
    H = 40
    Mutant = "blind"

INVARIANT
    _inv

CHECK_DEADLOCK
    \* CHECK_DEADLOCK off because of PROPERTY or INVARIANT above.
    FALSE

INIT
    _init

NEXT
    _next

CONSTANT
    _TETrace <- _trace

ALIAS
    _expression
=============================================================================
\* Generated on Tue Oct 06 21:42:45 WEST 2026