#pragma once
#include "lib/fn_heuristics.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Herd7TranspilerResult — generated AArch64 litmus tests for every surviving
// function pair.
//
// For each pair (anchorFn, partnerFn) that was NOT filtered by the heuristics
// (i.e. the two functions may run concurrently), a herd7 AArch64 litmus test
// is generated that models the shared-memory accesses in both functions with
// the ordering instructions present in the IR between them.
//
// The litmus tests can be fed directly to the herd7 simulator to determine
// whether the observed outcomes are allowed or forbidden under the AArch64
// memory model.
// ---------------------------------------------------------------------------
struct Herd7TranspilerResult {
    // filename -> litmus text  (filename has no directory prefix, ends in .litmus)
    std::map<std::string, std::string> litmusFiles;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

// ---------------------------------------------------------------------------
// Herd7TranspilerPass — LLVM module analysis pass.
//
// Depends on FnHeuristicsFilterPass (and transitively AnchorPass) being
// registered in the same MAM.
//
// Algorithm:
//   1. Obtain FnHeuristicsFilterResult.  Each entry in byAnchorFn is a pair
//      that passed all heuristic filters and is therefore considered to be
//      potentially concurrent.
//   2. For each surviving FunctionPair:
//      a. Map shared objects to abstract litmus variables (x, y, z, …).
//      b. Translate the anchor-function bracket (before → ordering → anchor
//         → ordering → after) to AArch64 Herd7 instructions for thread P0.
//      c. Translate the partner-function accesses (including any ordering
//         instructions between them, sourced from the IR) to instructions
//         for thread P1.
//      d. Assemble a .litmus file (name, init block, P0 | P1, exists clause).
//   3. Return the Herd7TranspilerResult map.
//
// Usage:
//   MAM.registerPass([] { return Herd7TranspilerPass(); });
//   const Herd7TranspilerResult &R = MAM.getResult<Herd7TranspilerPass>(M);
//   for (auto &[name, text] : R.litmusFiles)
//       writeFile(outDir / name, text);
// ---------------------------------------------------------------------------
class Herd7TranspilerPass
    : public llvm::AnalysisInfoMixin<Herd7TranspilerPass>
{
    friend struct llvm::AnalysisInfoMixin<Herd7TranspilerPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = Herd7TranspilerResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};
