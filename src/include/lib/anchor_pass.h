#pragma once
#include "lib/anchors.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AnchorHit — one detected memory-ordering anchor in a function body
// ---------------------------------------------------------------------------
struct AnchorHit {
    std::string       label;    // display label:
                                //   asm/volatile → mnemonic / "load volatile ptr"
                                //   external fn  → bare function name
                                //   ext anchor   → "fnName (ext)"
    anchors::Ordering ordering; // ordering strength
    std::string       site;     // IR site: "asm sideeffect", "@callee",
                                //           "load volatile", "store volatile"
    std::string       asmInst;  // non-empty for external-function hits:
                                //   the representative asm instruction that
                                //   corresponds to the ordering
                                //   (from ext_fn_summary or derived from kExtAnchors)
};

// ---------------------------------------------------------------------------
// AnchorResult — pass output: per-function list of unique anchor hits
//
// Only functions that contain at least one anchor are present in the map.
// Within each function the hits are deduplicated by label (so if ldaddal
// appears in 10 basic blocks it is reported once).
// ---------------------------------------------------------------------------
struct AnchorResult {
    // Key: LLVM function name (mangled).
    std::map<std::string, std::vector<AnchorHit>> perFunction;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

// ---------------------------------------------------------------------------
// AnchorPass — LLVM module analysis pass
//
// For every defined function in the module the pass scans:
//   1. CallInst / CallBrInst with InlineAsm callee
//      → parse asm string, match mnemonics against anchors::kAsmAnchors
//   2. LoadInst with volatile flag  → compiler-only (READ_ONCE / rcu_dereference)
//   3. StoreInst with volatile flag → compiler-only (WRITE_ONCE)
//   4. CallInst to a Declaration (external function, non-intrinsic)
//      → first: longest-prefix match against anchors::kExtAnchors (curated)
//      → then:  exact O(1) hashtable lookup in anchors::kExtFnSummary (10 280 entries)
//
// Usage in a ModuleAnalysisManager:
//   MAM.registerPass([] { return AnchorPass(); });
//   const AnchorResult &R = MAM.getResult<AnchorPass>(M);
// ---------------------------------------------------------------------------
class AnchorPass : public llvm::AnalysisInfoMixin<AnchorPass>
{
    friend struct llvm::AnalysisInfoMixin<AnchorPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = AnchorResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};
