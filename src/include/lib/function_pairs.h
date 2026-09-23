#pragma once
#include "lib/causal_detect.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PartnerAccess — one IR instruction in the partner function that accesses
// a shared object identified by the bracket (channel or causal payload).
// ---------------------------------------------------------------------------
struct PartnerAccess {
    std::string irInst;    // one-line IR text of the instruction
    std::string objDesc;   // "@name" or "(obj:N)" — the shared object hit
};

// ---------------------------------------------------------------------------
// FunctionPair — a matched pair of functions that together represent both
// sides of a non-blocking memory-ordering pattern.
//
//   anchorFn         : the function that contains the ordering anchor (from
//                      CausalDetectPass).
//   partnerFn        : a different function that accesses the same shared
//                      objects, constituting the "other side" of the pattern.
//   bracket          : the AnchorBracket from anchorFn that drove the match.
//   partnerAccesses  : IR instructions in partnerFn that access channel or
//                      causal shared objects from this bracket.
// ---------------------------------------------------------------------------
struct FunctionPair {
    std::string   anchorFn;
    std::string   partnerFn;
    AnchorBracket bracket;
    std::vector<PartnerAccess> partnerAccesses;
};

// ---------------------------------------------------------------------------
// FunctionPairsResult — pass output
// ---------------------------------------------------------------------------
struct FunctionPairsResult {
    // Key: anchor function name.  Value: all pairs found for that function.
    std::map<std::string, std::vector<FunctionPair>> byAnchorFn;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

// ---------------------------------------------------------------------------
// FunctionPairsPass — LLVM module analysis pass
//
// Depends on CausalDetectPass (registered in the same MAM).
//
// Algorithm:
//   1. Obtain CausalDetectResult for all anchor brackets.
//   2. Rebuild SVF Andersen PTA (CausalDetect tears it down after its run).
//   3. For each defined function G in the module compute its "footprint":
//      the set of base shared-object NodeIDs that G reads or writes via
//      explicit load/store instructions.
//   4. For each anchor bracket B in anchor function F:
//
//      Case A — B has an anchorAlloc (Store / RMW / ext anchor):
//        channel_objs  = base objects that anchorAlloc.ptrVal points to
//        causal_objs   = base objects that before.ptrVal and/or after.ptrVal
//                        point to
//        Match: G must intersect channel_objs AND (causal_objs OR causal empty)
//
//      Case B — B has no anchorAlloc (ThreadBarrier: dmb/dsb):
//        before_objs = base objects that before.ptrVal points to
//        after_objs  = base objects that after.ptrVal  points to
//        Match: G must intersect both before_objs AND after_objs
//
//   5. Every matched G != F is recorded as a FunctionPair.
// ---------------------------------------------------------------------------
class FunctionPairsPass : public llvm::AnalysisInfoMixin<FunctionPairsPass>
{
    friend struct llvm::AnalysisInfoMixin<FunctionPairsPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = FunctionPairsResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};
