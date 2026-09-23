--------------------------- MODULE MBCheckIR ---------------------------
(***************************************************************************)
(* MBCheckIR --- TLA+ specification of the MBCheck transpiler abstraction. *)
(*                                                                         *)
(* This module defines the eight representativeness properties P1--P8 that *)
(* the reduced IR (the generated AArch64 herd7 litmus test) must satisfy   *)
(* with respect to the original LLVM IR.  Each property is encoded as a    *)
(* state predicate over the abstract pair (original_ir, reduced_litmus).   *)
(*                                                                         *)
(* This spec is the formal counterpart to the C++ implementation in        *)
(* src/lib/litmus_verifier.cpp: every predicate here has a direct          *)
(* implementation as a member function on `LitmusVerifierPass`.            *)
(*                                                                         *)
(* Run with TLC using MBCheckIR.cfg to model-check P1--P8 on bounded       *)
(* instances of the abstraction.  The C++ verifier is then exercised on    *)
(* the full sweep to obtain empirical evidence.                            *)
(***************************************************************************)

EXTENDS Naturals, FiniteSets, Sequences, OrderingLattice

CONSTANTS
    MaxEvents,        \* upper bound on |original_events|  (typically 8)
    MaxObjects,       \* upper bound on |Objects|          (typically 4)
    Threads           \* set of thread identifiers         ({"P0","P1"})

ASSUME /\ MaxEvents  \in Nat /\ MaxEvents  > 0
       /\ MaxObjects \in Nat /\ MaxObjects > 0
       /\ Threads = { "P0", "P1" }

(***************************************************************************)
(* Abstract event identifiers and abstract symbols (the "x, y, z" of the   *)
(* litmus).  EventId values stand for instruction positions in the         *)
(* original linearisation; Symbol values stand for entries in VarMap.      *)
(***************************************************************************)
EventId == 1 .. MaxEvents
Symbol  == 1 .. MaxObjects

(***************************************************************************)
(* AccessKind:                                                             *)
(*   "Load"    - plain load                                                *)
(*   "Store"   - plain store                                               *)
(*   "Barrier" - DMB/DSB/anchor barrier (no address)                       *)
(*   "Anchor"  - the bracket anchor itself (release-store / acquire-load / *)
(*               full-barrier RMW / external-fn ordering call)             *)
(***************************************************************************)
AccessKind == { "Load", "Store", "Barrier", "Anchor" }

(***************************************************************************)
(* An event = (id, thread, kind, symbol_or_none, ordering).                *)
(*   id       - integer position in the original IR                        *)
(*   thread   - which litmus thread it ends up on ("P0" or "P1" or "none") *)
(*   kind     - AccessKind                                                 *)
(*   symbol   - 0 = no address (barrier), else \in Symbol                  *)
(*   ordering - element of OrderingClass                                   *)
(***************************************************************************)
Event == [ id       : EventId,
           thread   : Threads \cup {"none"},
           kind     : AccessKind,
           symbol   : 0 .. MaxObjects,
           ordering : OrderingClass ]

(***************************************************************************)
(* The two pillars of state:                                               *)
(*                                                                         *)
(*   original : sequence of events from the LLVM IR (linearised in po      *)
(*              order). Each event carries its true ordering class.        *)
(*                                                                         *)
(*   reduced  : sequence of events that appear in the generated litmus.    *)
(*              These are a sub-sequence of `original` (by index) -- the   *)
(*              transpiler never invents new instructions (P4).            *)
(*                                                                         *)
(*   pta_alias : abstract may-alias relation on symbols, supplied by SVF.  *)
(*               P6 is checked against this relation.                      *)
(*                                                                         *)
(*   exists_clause : the parsed `~exists` / `exists` clause                *)
(*                   ( sync_event, payload_event_set ).                    *)
(***************************************************************************)
VARIABLES original, reduced, pta_alias, exists_clause

vars == << original, reduced, pta_alias, exists_clause >>

(***************************************************************************)
(* Position of an event in a sequence (1-indexed).  Undefined (-1) if      *)
(* absent.                                                                 *)
(***************************************************************************)
PosOf(seq, e) ==
    LET P == { i \in 1..Len(seq) : seq[i] = e }
    IN IF P = {} THEN -1
                 ELSE CHOOSE i \in P : TRUE

(***************************************************************************)
(* Helper: the set of indices used by sequence `s` in the original.        *)
(***************************************************************************)
IndicesOf(s) == { s[i].id : i \in 1..Len(s) }

(***************************************************************************)
(* Helper: an event is a barrier kind?                                     *)
(***************************************************************************)
IsBarrierKind(k) == k \in { "Barrier", "Anchor" }

(*=========================================================================*)
(* PROPERTY P1 -- Program-order embedding.                                  *)
(*                                                                         *)
(* Every pair of retained events appears in the litmus in the same         *)
(* relative order as in the original IR.  Equivalently: the sequence of    *)
(* event ids in `reduced` is strictly increasing.                          *)
(*                                                                         *)
(* In the C++ verifier this is checked by extracting beforeIdx, anchorIdx, *)
(* afterIdx for the anchor thread (and the partner access indices for the *)
(* partner thread) and asserting strict monotonicity.                      *)
(*=========================================================================*)
P1_PO_Embedding ==
    \A i, j \in 1..Len(reduced) :
        i < j => reduced[i].id < reduced[j].id

(*=========================================================================*)
(* PROPERTY P2 -- Anchor ordering fidelity.                                *)
(*                                                                         *)
(* Every reduced event of kind "Anchor" carries an ordering class no       *)
(* weaker than its original-IR counterpart.  In practice the transpiler    *)
(* preserves the exact class via `barrierEquiv` / `emitAnchor`; the spec   *)
(* allows strengthening to give the implementation flexibility.            *)
(*                                                                         *)
(* In the C++ verifier this is checked by re-deriving the expected         *)
(* mnemonic from `anchor.ordering` via the OrderingToBarrier / barrierEquiv*)
(* tables and parsing the litmus thread to confirm a token of equal-or-    *)
(* greater strength appears at the anchor position.                        *)
(*=========================================================================*)
SameId(x, y) == x.id = y.id

P2_AnchorFidelity ==
    \A a \in { e \in Range(reduced) : e.kind = "Anchor" } :
        \A o \in { e \in Range(original) : SameId(e, a) } :
            OrderingLE(o.ordering, a.ordering)

\* Range(seq) is not a built-in in basic TLA+; define it here.
Range(seq) == { seq[i] : i \in 1..Len(seq) }

(*=========================================================================*)
(* PROPERTY P3 -- Witness reachable-from closure.                          *)
(*                                                                         *)
(* For every symbol that is written by the anchor side (i.e. appears as a *)
(* Store/Anchor in P0) and observed by P1, the relevant load event must   *)
(* be present in `reduced` (so the litmus can exhibit the                  *)
(* synchronisation).  Concretely: every symbol appearing in the exists    *)
(* clause must have at least one load/anchor event in P1's reduced slice. *)
(*=========================================================================*)
P0Writes ==
    { e.symbol : e \in { x \in Range(reduced) :
                        /\ x.thread = "P0"
                        /\ x.kind \in {"Store","Anchor"}
                        /\ x.symbol /= 0 } }

P1Reads ==
    { e.symbol : e \in { x \in Range(reduced) :
                        /\ x.thread = "P1"
                        /\ x.kind \in {"Load","Anchor"}
                        /\ x.symbol /= 0 } }

P3_RfClosure ==
    \A s \in (P0Writes \cap P1Reads) :
        \E e \in Range(reduced) :
            /\ e.thread = "P1"
            /\ e.symbol = s
            /\ e.kind \in {"Load","Anchor"}

(*=========================================================================*)
(* PROPERTY P4 -- No spurious hb-addition.                                 *)
(*                                                                         *)
(* Every event appearing in `reduced` must have its id present in the     *)
(* original IR.  The transpiler is a pure filter: it never invents        *)
(* barriers, loads, or stores that have no counterpart in the source.    *)
(*                                                                         *)
(* In the C++ verifier this is checked by re-running                       *)
(* `collectOrderingBetween` on the same (before, anchor) and               *)
(* (anchor, after) intervals and asserting that every DMB/DSB token in    *)
(* the litmus is in that re-computed set.                                  *)
(*=========================================================================*)
P4_NoSpuriousHbAdd ==
    \A e \in Range(reduced) :
        \E o \in Range(original) : o.id = e.id

(*=========================================================================*)
(* PROPERTY P5 -- No spurious hb-removal (semantic dominance).             *)
(*                                                                         *)
(* For each interval (before, anchor) and (anchor, after) in P0 -- the    *)
(* indices that bracket each retained event -- every original-IR barrier  *)
(* that has been *omitted* from the litmus must be subsumed by some       *)
(* retained event whose ordering >= the omitted barrier's ordering.       *)
(*                                                                         *)
(* The SC case is provable from SCDominance: any omitted barrier b with   *)
(* OrderingLE(b, SC) is subsumed by an SC anchor in the same interval.   *)
(*                                                                         *)
(* The CompilerOnly case is NOT provable; the verifier marks it as        *)
(* "unchecked" rather than failing.                                       *)
(*=========================================================================*)
OmittedBarriers ==
    { o \in Range(original) :
        /\ IsBarrierKind(o.kind)
        /\ ~\E r \in Range(reduced) : r.id = o.id }

SubsumedByRetained(o) ==
    \E r \in Range(reduced) :
        /\ r.thread = o.thread
        /\ IsBarrierKind(r.kind)
        /\ OrderingLE(o.ordering, r.ordering)

P5_NoSpuriousHbRemove ==
    \A o \in OmittedBarriers :
        \/ SubsumedByRetained(o)
        \/ o.ordering = "CompilerOnly"      \* unchecked case
        \/ o.ordering = "Relaxed"           \* no hb to remove anyway
        \/ o.ordering = "None"

(*=========================================================================*)
(* PROPERTY P6 -- Alias faithfulness.                                      *)
(*                                                                         *)
(* Two events that share a litmus symbol must may-alias according to the  *)
(* SVF PTA oracle.  Equivalently: the transpiler never collapses two      *)
(* events to the same symbol unless PTA agrees they may alias.            *)
(*                                                                         *)
(* The reverse direction (PTA agrees, but events get different symbols)   *)
(* is allowed -- it represents acceptable conservatism in the symbol-     *)
(* assignment.                                                            *)
(*=========================================================================*)
P6_AliasFaithful ==
    \A e1, e2 \in Range(reduced) :
        (e1.symbol = e2.symbol /\ e1.symbol /= 0 /\ e1 /= e2)
            => pta_alias[e1.id][e2.id]

(*=========================================================================*)
(* PROPERTY P7 -- Exists-clause structural soundness.                      *)
(*                                                                         *)
(* The exists clause must take one of the two valid forms                  *)
(*   form (i):  `exists (1:R = 1)`                                         *)
(*   form (ii): `~exists (1:R_sync = 1 /\ 1:R_pay1 = 0 /\ ...)`            *)
(* and the registers it mentions must correspond to events in P1.         *)
(*                                                                         *)
(* In the spec we model the clause as a record                             *)
(*   [ kind     |-> "exists" | "notexists",                                *)
(*     sync_id  |-> EventId,                                               *)
(*     pay_ids  |-> SUBSET EventId ]                                       *)
(*=========================================================================*)
P7_ExistsClause ==
    /\ exists_clause.kind \in { "exists", "notexists" }
    /\ \E e \in Range(reduced) :
            /\ e.id = exists_clause.sync_id
            /\ e.thread = "P1"
            /\ e.kind \in { "Load", "Anchor" }
    /\ \A pid \in exists_clause.pay_ids :
            \E e \in Range(reduced) :
                /\ e.id = pid
                /\ e.thread = "P1"
                /\ e.kind \in { "Load", "Anchor" }
    /\ exists_clause.kind = "notexists" => exists_clause.pay_ids /= {}

(*=========================================================================*)
(* PROPERTY P8 -- Conservative approximation (derived theorem).            *)
(*                                                                         *)
(* P1 /\ P4 /\ P5 together imply that the reduced IR contains no more     *)
(* happens-before than the original.  Therefore any "No" verdict from     *)
(* herd7 on the reduced IR is a sound candidate violation in the original.*)
(*                                                                         *)
(* This is stated as a derived predicate; the verifier does not need to   *)
(* check it independently.                                                *)
(*=========================================================================*)
P8_Conservative == P1_PO_Embedding /\ P4_NoSpuriousHbAdd /\ P5_NoSpuriousHbRemove

(*=========================================================================*)
(* Conjunction of all properties.  This is the invariant for TLC to check. *)
(*=========================================================================*)
AllProperties ==
    /\ P1_PO_Embedding
    /\ P2_AnchorFidelity
    /\ P3_RfClosure
    /\ P4_NoSpuriousHbAdd
    /\ P5_NoSpuriousHbRemove
    /\ P6_AliasFaithful
    /\ P7_ExistsClause
    /\ P8_Conservative

(***************************************************************************)
(* Type invariant -- shape of the four state variables.                    *)
(***************************************************************************)
TypeOK ==
    /\ original \in Seq(Event)
    /\ reduced  \in Seq(Event)
    /\ Len(original) <= MaxEvents
    /\ Len(reduced)  <= Len(original)
    /\ pta_alias \in [ EventId -> [ EventId -> BOOLEAN ] ]
    /\ exists_clause \in [ kind     : { "exists", "notexists" },
                           sync_id  : EventId,
                           pay_ids  : SUBSET EventId ]

(***************************************************************************)
(* Operational model: the transpiler is modelled as a single atomic step   *)
(* that produces `reduced`, `pta_alias`, `exists_clause` from `original`.  *)
(* For model checking we treat the system as a state machine whose only    *)
(* action is "Transpile": any valid reduction satisfying the structural    *)
(* constraints below may be produced.                                      *)
(***************************************************************************)
ValidReduction(orig, red) ==
    /\ \A i \in 1..Len(red) :
            \E j \in 1..Len(orig) :
                /\ orig[j] = red[i]
    /\ P1_PO_Embedding'      \* enforce po-embedding at the transition

Init ==
    /\ original \in Seq(Event)
    /\ Len(original) \in 1..MaxEvents
    /\ reduced = << >>
    /\ pta_alias \in [ EventId -> [ EventId -> BOOLEAN ] ]
    /\ exists_clause = [ kind |-> "exists", sync_id |-> 1, pay_ids |-> {} ]

Transpile ==
    /\ reduced = << >>
    /\ \E r \in Seq(Event) :
            /\ Len(r) <= Len(original)
            /\ \A i \in 1..Len(r) :
                    \E j \in 1..Len(original) : original[j] = r[i]
            /\ reduced' = r
    /\ \E ec \in [ kind : { "exists", "notexists" },
                   sync_id : EventId,
                   pay_ids : SUBSET EventId ] :
            exists_clause' = ec
    /\ UNCHANGED << original, pta_alias >>

Next == Transpile

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* The invariant TLC should check.                                         *)
(***************************************************************************)
Inv ==
    (reduced /= << >>) => AllProperties

=============================================================================
