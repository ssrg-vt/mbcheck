#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace std;

struct FSPTAResult
{
    using NodeId = uint32_t;

    /// Inverted points-to relation: memory-object ID → pointer IDs.
    /// Only GlobalObjVar and HeapObjVar objects are included;
    /// trivial self-pointers (GlobalValVar → its own GlobalObjVar) are excluded.
    map<NodeId, vector<NodeId>> reversePointsTo;
    /// Human-readable description for every node that appears above.
    map<NodeId, string> nodeDescriptions;

    bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &,
                    llvm::ModuleAnalysisManager::Invalidator &)
    {
        return false;
    }
};

class FSPTAPass
    : public llvm::AnalysisInfoMixin<FSPTAPass>
{
    friend struct llvm::AnalysisInfoMixin<FSPTAPass>;
    static llvm::AnalysisKey Key;

public:
    using Result = FSPTAResult;

    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};