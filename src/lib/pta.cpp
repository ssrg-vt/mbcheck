#include "lib/pta.h"
#include "lib/allocatorPre.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/FlowSensitive.h"
#include "WPA/Steensgaard.h"
#include "WPA/Andersen.h"
#include "WPA/VersionedFlowSensitive.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Options.h"

using namespace llvm;
using namespace SVF;

AnalysisKey FSPTAPass::Key;

FSPTAResult FSPTAPass::run(Module &M, ModuleAnalysisManager &)
{
    FSPTAResult result;

    // suppress SVF's stats.
    const_cast<Option<bool> &>(Options::PStat).setValue(false);

    // Rewrite kernel allocator calls (kmalloc, vmalloc, etc.) to malloc so
    // that SVF recognises them as heap allocations and creates HeapObjVars.
    normaliseKernelAllocators(M);

    LLVMModuleSet::buildSVFModule(M);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();

    // Compiler-generated module metadata (globals) should be suppressed from output.
    llvm::DenseSet<const llvm::Value *> usedInFunctions;
    for (const auto &GV : M.globals())
    {
        for (const auto *U : GV.users())
        {
            // Direct use by an instruction.
            if (llvm::isa<llvm::Instruction>(U))
            {
                usedInFunctions.insert(&GV);
                break;
            }
            // Indirect use via a ConstantExpr folded into an instruction.
            if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U))
            {
                for (const auto *CEU : CE->users())
                {
                    if (llvm::isa<llvm::Instruction>(CEU))
                    {
                        usedInFunctions.insert(&GV);
                        break;
                    }
                }
            }
        }
    }

    // Note: FlowSensitive does not precisely track across init/exit function
    // boundaries in device drivers.  AndersenWaveDiff is more conservative.
    auto *pta = AndersenWaveDiff::createAndersenWaveDiff(pag);

    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        NodeID nid     = it->first;
        SVFVar *svfVar = it->second;

        if (!llvm::isa<ValVar>(svfVar) || !svfVar->isPointer())
            continue;

        const PointsTo &pts = pta->getPts(nid);
        for (NodeID objId : pts)
        {
            SVFVar *objNode = pag->getGNode(objId);

            bool isGlobal = llvm::isa<GlobalObjVar>(objNode);
            bool isHeap   = llvm::isa<HeapObjVar>(objNode);
            if (!isGlobal && !isHeap)
                continue;

            // if (llvm::isa<ValVar>(svfVar) &&
            //     llvm::cast<ValVar>(svfVar)->getICFGNode() ==
            //         llvm::cast<BaseObjVar>(objNode)->getICFGNode())
            //     continue;
            
            // Suppress globals that are pure module metadata.
            if (isGlobal)
            {
                const llvm::Value *llvmVal =
                    LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(objNode);
                if (!llvmVal || !usedInFunctions.count(llvmVal))
                    continue;
            }

            auto oid = static_cast<FSPTAResult::NodeId>(objId);
            auto pid = static_cast<FSPTAResult::NodeId>(nid);

            result.reversePointsTo[oid].push_back(pid);

            result.nodeDescriptions.emplace(
                oid, pag->getGNode(objId)->toString());
            result.nodeDescriptions.emplace(
                pid, pag->getGNode(nid)->toString());
        }
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();

    return result;
}