------------------------- MODULE OrderingLattice -------------------------
(***************************************************************************)
(* OrderingLattice  ---  shared ordering-strength lattice for MBCheck.     *)
(*                                                                         *)
(* This module mirrors `anchors::Ordering` (src/include/lib/anchors.h) and *)
(* `orderingToHerd7Barrier` / `mnemonicToHerd7` / `barrierEquiv`           *)
(* (src/lib/herd7_transpiler.cpp).  It is referenced by MBCheckIR for      *)
(* properties P2 (anchor fidelity) and P5 (no spurious hb-removal).        *)
(*                                                                         *)
(* The lattice:                                                            *)
(*                                                                         *)
(*       SC                                                                *)
(*      /  \                                                               *)
(*  Acquire  Release                                                       *)
(*      \  /                                                               *)
(*    CompilerOnly                                                         *)
(*        |                                                                *)
(*      Relaxed                                                            *)
(*        |                                                                *)
(*       None                                                              *)
(*                                                                         *)
(* SC dominates every other class.  Acquire and Release are incomparable   *)
(* but both strictly dominate CompilerOnly.  CompilerOnly dominates        *)
(* Relaxed; Relaxed dominates None.                                        *)
(***************************************************************************)

EXTENDS Naturals, FiniteSets, Sequences

OrderingClass == { "SC", "Acquire", "Release", "CompilerOnly",
                   "Relaxed", "None" }

(***************************************************************************)
(* Numeric rank used only as a tiebreaker for total comparisons.  The      *)
(* partial-order relation OrderingLE is defined explicitly below to        *)
(* preserve the Acquire/Release incomparability.                           *)
(***************************************************************************)
OrderingRank(o) ==
    CASE o = "SC"           -> 5
      [] o = "Acquire"      -> 4
      [] o = "Release"      -> 3
      [] o = "CompilerOnly" -> 2
      [] o = "Relaxed"      -> 1
      [] o = "None"         -> 0

(***************************************************************************)
(* Partial-order relation: a is weaker-or-equal-to b.                      *)
(* This mirrors the C++ "ordering >= other" intent used throughout the     *)
(* transpiler (e.g. `oi.ordering > it->second.ordering` in                 *)
(* collectOrderingBetween).                                                *)
(***************************************************************************)
OrderingLE(a, b) ==
    \/ a = b
    \/ b = "SC"
    \/ a = "None"
    \/ a = "Relaxed"      /\ b \in { "Relaxed", "CompilerOnly",
                                     "Acquire", "Release", "SC" }
    \/ a = "CompilerOnly" /\ b \in { "CompilerOnly", "Acquire",
                                     "Release", "SC" }

(***************************************************************************)
(* `o` is strictly weaker than `p`.                                        *)
(***************************************************************************)
OrderingLT(a, b) == OrderingLE(a, b) /\ a /= b

(***************************************************************************)
(* SC dominance axiom (used to prove P5 for SC anchors).  An SC anchor in  *)
(* P0 establishes hb between every po-predecessor and every po-successor   *)
(* of the anchor, so any intermediate barrier with ordering <= SC is       *)
(* redundant and may be omitted without removing an hb edge.               *)
(*                                                                         *)
(* In TLA+ this is an `ASSUME` -- it abstracts the AArch64 memory-model    *)
(* fact that DMB ISH is a full barrier.  TLC does not prove it; the C++    *)
(* verifier trusts the same fact.                                          *)
(***************************************************************************)
ASSUME SCDominance ==
    \A o \in OrderingClass : OrderingLE(o, "SC")

(***************************************************************************)
(* Herd7 barrier token corresponding to an ordering class.  Mirrors        *)
(* `orderingToHerd7Barrier` in herd7_transpiler.cpp.                       *)
(*                                                                         *)
(* Returns "" (empty token) for CompilerOnly / Relaxed / None -- those     *)
(* classes do not emit any hardware barrier.                               *)
(***************************************************************************)
OrderingToBarrier(o) ==
    CASE o = "SC"      -> "DMB_ISH"
      [] o = "Acquire" -> "DMB_LD"
      [] o = "Release" -> "DMB_ST"
      [] OTHER         -> ""

(***************************************************************************)
(* The set of pure-barrier tokens that can appear in a generated litmus.   *)
(* Mirrors the `kPureBarriers` array in `emitAnchor`.                      *)
(***************************************************************************)
PureBarriers == { "DMB_ISH", "DMB_ST", "DMB_LD", "DMB_OSH", "DSB_SY" }

(***************************************************************************)
(* Ordering class associated with a pure-barrier token.  Used by P4 (no    *)
(* spurious hb-addition): every barrier in the litmus must be derivable    *)
(* from some original-IR ordering source whose class >= BarrierOrdering(b).*)
(***************************************************************************)
BarrierOrdering(b) ==
    CASE b = "DMB_ISH" -> "SC"
      [] b = "DMB_OSH" -> "SC"
      [] b = "DSB_SY"  -> "SC"
      [] b = "DMB_LD"  -> "Acquire"
      [] b = "DMB_ST"  -> "Release"
      [] OTHER         -> "None"

=============================================================================
