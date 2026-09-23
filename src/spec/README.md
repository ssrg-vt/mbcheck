# `src/spec/` --- TLA+ specification of the MBCheck reduced-IR abstraction

This directory contains the formal specification of the eight
representativeness properties **P1--P8** that the generated AArch64 herd7
litmus test (the *reduced IR*) must satisfy with respect to the original
LLVM~IR.  Each predicate in the spec has a direct counterpart in the
C++ runtime verifier at
[`src/lib/litmus_verifier.cpp`](../lib/litmus_verifier.cpp).

## Files

| File | Purpose |
|------|---------|
| `OrderingLattice.tla` | LKMM ordering classes and the `OrderingLE` partial order; mirrors `anchors::Ordering`. |
| `MBCheckIR.tla`       | State model + predicates P1--P8 + the conservative-approximation theorem P8. |
| `MBCheckIR.cfg`       | TLC configuration (bounded constants for finite model checking). |

## Property summary

| # | Property                       | TLA+ predicate          | C++ check                       |
|---|--------------------------------|-------------------------|----------------------------------|
| **P1** | PO-embedding              | `P1_PO_Embedding`       | strictly increasing IR indices   |
| **P2** | Anchor ordering fidelity  | `P2_AnchorFidelity`     | ordering-lattice table lookup    |
| **P3** | rf-closure of witness     | `P3_RfClosure`          | P1 has load for every P0-write   |
| **P4** | No spurious hb-addition   | `P4_NoSpuriousHbAdd`    | barrier-set containment          |
| **P5** | No spurious hb-removal    | `P5_NoSpuriousHbRemove` | SC dominance + skipped CO case   |
| **P6** | Alias faithfulness        | `P6_AliasFaithful`      | objDesc equivalence + PTA query  |
| **P7** | Exists-clause soundness   | `P7_ExistsClause`       | regex on the emitted clause      |
| **P8** | Conservative approximation| `P8_Conservative`       | derived: `P1 /\ P4 /\ P5`        |

## Running TLC

Install TLC (any recent `tla2tools.jar`):

    wget https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar

Run the bounded model checker:

    cd src/spec
    java -cp /path/to/tla2tools.jar tlc2.TLC \
        -config MBCheckIR.cfg MBCheckIR.tla

Expected output:

    Model checking completed. No error has been found.

To localise a property, comment out `Inv` in `MBCheckIR.cfg` and
uncomment a single `Pn_*` predicate under `INVARIANTS`.

## Relationship to the C++ verifier

The verifier (`LitmusVerifierPass`, invoked via `mbcheck --verify <outdir>`)
implements the same predicates as decidable boolean checks over

  * the original `llvm::Module` (via flattened per-function instruction
    sequences),
  * the upstream pass results (`AnchorPass`, `CausalDetectPass`,
    `FunctionPairsPass`, `FnHeuristicsFilterPass`), and
  * the generated litmus text from `Herd7TranspilerPass`.

The TLA+ spec validates the **design** of those checks on bounded
instances; the verifier validates each **concrete output** on the full
kernel sweep.  An assertion failure in either pipeline points to the same
class of transpiler bug.

## What is *trusted* in the spec

Three axioms are stated as `ASSUME` and not proved:

  * **`SCDominance`**: `\A o \in OrderingClass : OrderingLE(o, "SC")` ---
    the AArch64 fact that DMB ISH establishes hb across every po-ordered
    pair.  Justified by the AArch64 reference manual.
  * **PTA soundness**: the `pta_alias` relation is provided as input
    and assumed to be a sound may-alias oracle.  Justified by Andersen
    soundness (SVF reference).
  * **herd7 oracle**: the herd7 verdict on the reduced IR is taken as
    ground truth for the AArch64 axiomatic model.  Justified by the
    herd7/cat tooling reference.

P8 (conservative approximation) is then *derived* from P1, P4, P5 and
the three axioms; the verifier does not need a separate runtime check.
