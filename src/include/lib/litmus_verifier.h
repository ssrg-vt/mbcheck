#pragma once
// litmus_verifier.h — runtime checker for the eight representativeness
// properties (P1–P8) of the MBCheck reduced-IR abstraction.
//
// Each property is the C++ counterpart of a TLA+ predicate defined in
// src/spec/MBCheckIR.tla.  See src/spec/README.md for the formal model.
//
//   P1  PO-embedding             — strictly increasing original-IR indices
//   P2  Anchor ordering fidelity — emitted token ≥ anchor.ordering
//   P3  rf-closure of witness    — every P0-write symbol has a P1 load
//   P4  No spurious hb-addition  — every emitted barrier ∈ source IR
//   P5  No spurious hb-removal   — omitted barriers subsumed by retained anchor
//                                  (SC dominance; CompilerOnly is "skipped")
//   P6  Alias faithfulness       — same litmus var ⇒ same SVF objDesc
//   P7  Exists-clause soundness  — clause matches the MP template
//   P8  Conservative approx      — derived: P1 ∧ P4 ∧ P5
//
// LitmusVerifierPass consumes the upstream pipeline (AnchorPass,
// CausalDetectPass, FunctionPairsPass, FnHeuristicsFilterPass,
// Herd7TranspilerPass) and emits one PerLitmusReport per generated test.
//
// Aggregate counts can be printed by the driver (see runVerifier in
// src/driver/mbcheck.cpp, behind the --verify CLI flag).

#include "lib/fn_heuristics.h"
#include "lib/herd7_transpiler.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PropertyCheck — outcome of one Pn check on one litmus test
// ---------------------------------------------------------------------------
struct PropertyCheck {
    enum class Status : uint8_t {
        Pass    = 0, // check applies and holds
        Fail    = 1, // check applies and is violated
        Skipped = 2, // check does not apply (e.g. P5 on a CompilerOnly anchor)
    };

    Status      status = Status::Pass;
    std::string detail; // explanation; non-empty on Fail or Skipped

    bool ok()      const { return status == Status::Pass; }
    bool failed()  const { return status == Status::Fail; }
    bool skipped() const { return status == Status::Skipped; }

    static PropertyCheck pass()                              { return {Status::Pass, ""}; }
    static PropertyCheck fail(std::string why)               { return {Status::Fail,    std::move(why)}; }
    static PropertyCheck skip(std::string why)               { return {Status::Skipped, std::move(why)}; }
};

// ---------------------------------------------------------------------------
// PerLitmusReport — verification outcome for one generated .litmus file
// ---------------------------------------------------------------------------
struct PerLitmusReport {
    std::string litmusName;   // basename of the .litmus file (no extension)
    std::string anchorFn;
    std::string partnerFn;

    PropertyCheck p1, p2, p3, p4, p5, p6, p7, p8;

    bool allOk() const {
        return p1.ok() && p2.ok() && p3.ok() && p4.ok() &&
               (p5.ok() || p5.skipped()) && p6.ok() && p7.ok() && p8.ok();
    }
};

// ---------------------------------------------------------------------------
// LitmusVerifierResult — aggregate output of the pass
// ---------------------------------------------------------------------------
struct LitmusVerifierResult {
    std::vector<PerLitmusReport> reports;

    // Aggregates (filled by run()).
    int total = 0;
    struct Counts { int pass = 0, fail = 0, skip = 0; };
    Counts p1, p2, p3, p4, p5, p6, p7, p8;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

// ---------------------------------------------------------------------------
// LitmusVerifierPass — module analysis pass.
//
// Requires the entire upstream pipeline to be registered in the same
// ModuleAnalysisManager:
//
//   MAM.registerPass([] { return AnchorPass(); });
//   MAM.registerPass([] { return CausalDetectPass(); });
//   MAM.registerPass([] { return FunctionPairsPass(); });
//   MAM.registerPass([] { return FnHeuristicsFilterPass(); });
//   MAM.registerPass([] { return Herd7TranspilerPass(); });
//   MAM.registerPass([] { return LitmusVerifierPass(); });
//
// The pass re-flattens each anchor / partner function body, re-locates the
// bracket and partner-access indices, and runs each Pn predicate against the
// litmus text produced by Herd7TranspilerPass.
// ---------------------------------------------------------------------------
class LitmusVerifierPass
    : public llvm::AnalysisInfoMixin<LitmusVerifierPass>
{
    friend struct llvm::AnalysisInfoMixin<LitmusVerifierPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = LitmusVerifierResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};
