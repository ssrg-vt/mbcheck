#pragma once
#include "lib/anchor_pass.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SharedAccess — a single load or store to a shared (heap/global) allocation
// ---------------------------------------------------------------------------
struct SharedAccess {
    enum class Kind { Load, Store };
    Kind        kind;
    std::string ptrDesc;  // SVF description of the pointer operand
    std::string objDesc;  // SVF description of the pointee object
    std::string irInst;   // one-line IR text of the instruction
    // Raw LLVM pointer operand — populated by CausalDetectPass so that
    // FunctionPairsPass can query SVF PTA without re-parsing irInst.
    const llvm::Value *ptrVal = nullptr;
};

// ---------------------------------------------------------------------------
// AnchorBracket — one anchor and the shared accesses that bracket it in the
// same function body (intraprocedural, linear program order)
//
//   before      : last shared access found *before* this anchor (if any)
//   anchorAlloc : shared object the anchor itself writes to (non-empty for
//                 Store/RMW/external anchors; empty for thread barriers like
//                 dmb/dsb which have no specific address).
//   after       : first shared access found *after* this anchor (if any)
//
// Output order: before → anchor(+anchorAlloc) → after
// ---------------------------------------------------------------------------
struct AnchorBracket {
    AnchorHit                   anchor;
    std::optional<SharedAccess> before;
    std::optional<SharedAccess> anchorAlloc;
    std::optional<SharedAccess> after;
};

// ---------------------------------------------------------------------------
// CausalDetectResult — per-function bracket results
// ---------------------------------------------------------------------------
struct CausalDetectResult {
    // Key: LLVM function name.  Only functions with at least one anchor that
    // has a shared access on at least one side are included.
    std::map<std::string, std::vector<AnchorBracket>> perFunction;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

// ---------------------------------------------------------------------------
// Strict-publish mode (Action 2 — kernel TP detection)
//
// When enabled, CausalDetectPass additionally synthesises "plain publish"
// brackets for functions that exhibit the message-passing (MP) shape WITHOUT
// any release barrier:
//
//   store payload, ptr @P     // plain store (payload)
//   store flag,    ptr @F     // plain store (publication flag) — NO STLR
//
// A synthetic AnchorBracket is created with:
//   anchor.label = "store (plain, no barrier)"
//   anchor.site  = "store plain"          (distinguishes from "asm sideeffect")
//   anchor.ordering = Ordering::Relaxed
//   anchor.kind  = AnchorKind::Store
//   anchorAlloc  = SharedAccess for the flag store (channel)
//   before       = SharedAccess for the payload store
//   after        = (none)
//
// Subsequent passes (function_pairs, fn_heuristics, herd7_transpiler) consume
// the synthetic bracket uniformly.  herd7_transpiler emits a plain `STR W,[X]`
// (no `STLR`, no DMB) for the publication, so on AArch64 the litmus exists
// clause `~exists (1:flag=1 /\ 1:payload=0)` is observable → herd7 reports
// `No` (ordering violated) — flagging the missing release barrier.
//
// Guarded by --strict-publish.  Default off because it generates many new
// candidate pairs in the real kernel.
// ---------------------------------------------------------------------------
void setStrictPublish(bool on);
bool strictPublish();

// ---------------------------------------------------------------------------
// CausalDetectPass — LLVM module analysis pass
//
// Requires AnchorPass and FSPTAPass to be registered in the same MAM.
//
// Algorithm (per function):
//   1. Obtain anchor sites from AnchorPass.
//   2. Use FSPTAPass to identify all LLVM Values that point to a shared
//      object (heap or non-metadata global).
//   3. Flatten each function into a linear instruction sequence (BB order).
//   4. For every anchor instruction site, scan backwards for the last shared
//      load/store and forwards for the first shared load/store.
// ---------------------------------------------------------------------------
class CausalDetectPass : public llvm::AnalysisInfoMixin<CausalDetectPass>
{
    friend struct llvm::AnalysisInfoMixin<CausalDetectPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = CausalDetectResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};
