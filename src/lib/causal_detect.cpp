#include "lib/phase_timer.h"
#include "lib/causal_detect.h"
#include "lib/anchor_pass.h"
#include "lib/pta.h"
#include "lib/allocatorPre.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"
#include "SVFIR/SVFVariables.h"
#include "SVFIR/SVFIR.h"
#include "Util/NodeIDAllocator.h"
#include "Util/Options.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <string>
#include <unordered_set>

using namespace llvm;
using namespace SVF;

AnalysisKey CausalDetectPass::Key;

// ---------------------------------------------------------------------------
// Helper: render an instruction to a compact one-line string
// ---------------------------------------------------------------------------
static std::string instToStr(const Instruction &I)
{
    std::string s;
    raw_string_ostream os(s);
    I.print(os);
    // Strip leading spaces and trim to first line
    size_t start = s.find_first_not_of(" \t");
    if (start != std::string::npos)
        s = s.substr(start);
    size_t nl = s.find('\n');
    if (nl != std::string::npos)
        s = s.substr(0, nl);
    return s;
}

// ---------------------------------------------------------------------------
// Helper: describe an SVF node concisely (verbose — used for ptrDesc)
// ---------------------------------------------------------------------------
static std::string nodeDesc(SVFIR *pag, NodeID nid)
{
    SVFVar *v = pag->getGNode(nid);
    return v ? v->toString() : std::to_string(nid);
}

// Helper: clean object description for display — "@name" or "(obj:N)"
// ---------------------------------------------------------------------------
static std::string cleanObjDesc(SVFIR *pag, NodeID nid)
{
    auto *msSet = SVF::LLVMModuleSet::getLLVMModuleSet();
    if (SVFVar *v = pag->getGNode(nid)) {
        if (msSet->hasLLVMValue(v)) {
            const llvm::Value *lv = msSet->getLLVMValue(v);
            if (lv->hasName())
                return "@" + lv->getName().str();
        }
    }
    return "(obj:" + std::to_string(nid) + ")";
}

// ---------------------------------------------------------------------------
// Pass implementation
// ---------------------------------------------------------------------------
CausalDetectResult CausalDetectPass::run(Module &M,
                                          ModuleAnalysisManager &MAM)
{
    mbtime::Scope _t("bracket_extraction");

    CausalDetectResult result;

    // ── 1. Run AnchorPass ───────────────────────────────────────────────────
    const AnchorResult &anchors = MAM.getResult<AnchorPass>(M);
    if (anchors.perFunction.empty())
        return result;

    // ── 2. Build SVF PAG + Andersen PTA ────────────────────────────────────
    const_cast<Option<bool> &>(Options::PStat).setValue(false);
    normaliseKernelAllocators(M);

    LLVMModuleSet::buildSVFModule(M);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();

    auto *pta = AndersenWaveDiff::createAndersenWaveDiff(pag);

    // ── 3. Determine "shared" object node IDs ──────────────────────────────
    // A shared object is a HeapObjVar or a GlobalObjVar that is actually used
    // inside some function body (not pure module metadata).
    llvm::DenseSet<const llvm::Value *> usedGlobals;
    for (const auto &GV : M.globals()) {
        for (const auto *U : GV.users()) {
            if (isa<Instruction>(U)) { usedGlobals.insert(&GV); break; }
            if (const auto *CE = dyn_cast<ConstantExpr>(U))
                for (const auto *CEU : CE->users())
                    if (isa<Instruction>(CEU)) { usedGlobals.insert(&GV); break; }
        }
    }

    // sharedObjIds: set of PAG object NodeIDs considered shared
    llvm::DenseSet<NodeID> sharedObjIds;
    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it) {
        NodeID nid     = it->first;
        SVFVar *svfVar = it->second;
        if (!isa<ObjVar>(svfVar))
            continue;
        bool isHeap   = isa<HeapObjVar>(svfVar);
        bool isGlobal = isa<GlobalObjVar>(svfVar);
        if (!isHeap && !isGlobal)
            continue;
        if (isGlobal) {
            auto *msSet = LLVMModuleSet::getLLVMModuleSet();
            if (!msSet->hasLLVMValue(svfVar))
                continue;
            const llvm::Value *llvmVal = msSet->getLLVMValue(svfVar);
            if (!usedGlobals.count(llvmVal))
                continue;
        }
        sharedObjIds.insert(nid);
    }

    // ── 4. Build a lookup: LLVM Value* → bool (points into shared obj?) ────
    // For every ValVar whose points-to set intersects sharedObjIds, record it.
    auto *msSet = LLVMModuleSet::getLLVMModuleSet();

    auto ptrIsShared = [&](const Value *ptrVal) -> std::optional<SharedAccess::Kind> {
        // We don't need the kind from PTA here, just whether the pointer
        // touches a shared object.  We return nothing from this helper;
        // the kind (Load/Store) is determined by the IR instruction.
        (void)ptrVal;
        return std::nullopt; // placeholder — see caller below
    };
    (void)ptrIsShared; // suppress unused-lambda warning

    // Helper: does ptrVal point to any shared object (or field thereof)?
    auto touchesShared = [&](const Value *ptrVal) -> bool {
        if (!msSet->hasValueNode(ptrVal))
            return false;
        NodeID nid = msSet->getValueNode(ptrVal);
        const PointsTo &pts = pta->getPts(nid);
        for (NodeID obj : pts) {
            if (sharedObjIds.count(obj))
                return true;
            // Also match field accesses (GepObjVar) into a shared base object
            if (const auto *gep = dyn_cast<GepObjVar>(pag->getGNode(obj)))
                if (sharedObjIds.count(gep->getBaseObj()->getId()))
                    return true;
        }
        return false;
    };

    // Helper: build a SharedAccess for a load or store to a shared pointer.
    auto makeAccess = [&](const Instruction &I,
                          const Value *ptrVal,
                          SharedAccess::Kind kind) -> SharedAccess {
        SharedAccess sa;
        sa.kind    = kind;
        sa.irInst  = instToStr(I);
        sa.ptrVal  = ptrVal;

        // Pointer node description
        if (msSet->hasValueNode(ptrVal)) {
            NodeID nid = msSet->getValueNode(ptrVal);
            sa.ptrDesc = nodeDesc(pag, nid);
            // Pick the first shared pointee (or its GEP base) for display
            const PointsTo &pts = pta->getPts(nid);
            for (NodeID obj : pts) {
                if (sharedObjIds.count(obj)) {
                    sa.objDesc = cleanObjDesc(pag, obj);
                    break;
                }
                if (const auto *gep = dyn_cast<GepObjVar>(pag->getGNode(obj))) {
                    if (sharedObjIds.count(gep->getBaseObj()->getId())) {
                        sa.objDesc = cleanObjDesc(pag, gep->getBaseObj()->getId());
                        break;
                    }
                }
            }
        } else {
            sa.ptrDesc = "(unknown ptr)";
        }
        return sa;
    };

    auto findAnchorAlloc = [&](const Instruction *inst) -> std::optional<SharedAccess> {
        // Accept both CallInst and CallBrInst.
        unsigned argCount = 0;
        auto getArg = [&](unsigned i) -> const Value * { return nullptr; };
        if (const auto *CI = dyn_cast<CallInst>(inst)) {
            argCount = CI->arg_size();
            auto getArgCI = [CI](unsigned i) -> const Value * { return CI->getArgOperand(i); };
            for (unsigned i = 0; i < argCount; ++i) {
                const Value *arg = CI->getArgOperand(i);
                const Value *ptrArg = arg;
                if (!ptrArg->getType()->isPointerTy()) {
                    const auto *P2I = dyn_cast<PtrToIntInst>(arg);
                    if (!P2I) continue;
                    ptrArg = P2I->getPointerOperand();
                }
                SharedAccess sa;
                sa.kind   = SharedAccess::Kind::Store; sa.irInst = "";
                sa.ptrVal = ptrArg;
                if (ptrArg->hasName()) sa.ptrDesc = ("@" + ptrArg->getName()).str();
                else { std::string t; raw_string_ostream o(t); ptrArg->printAsOperand(o, false); sa.ptrDesc = t; }
                if (msSet->hasValueNode(ptrArg)) {
                    NodeID nid = msSet->getValueNode(ptrArg);
                    const PointsTo &pts = pta->getPts(nid);
                    for (NodeID obj : pts) {
                        const SVFVar *c = nullptr;
                        if (sharedObjIds.count(obj)) c = pag->getGNode(obj);
                        else if (const auto *g = dyn_cast<GepObjVar>(pag->getGNode(obj)))
                            if (sharedObjIds.count(g->getBaseObj()->getId())) c = g->getBaseObj();
                        if (c && msSet->hasLLVMValue(c)) {
                            const llvm::Value *lv = msSet->getLLVMValue(c);
                            if (lv->hasName()) sa.objDesc = ("@" + lv->getName()).str();
                            break;
                        }
                    }
                }
                return sa;
            }
        } else if (const auto *CBR = dyn_cast<CallBrInst>(inst)) {
            for (unsigned i = 0; i < CBR->arg_size(); ++i) {
                const Value *arg = CBR->getArgOperand(i);
                const Value *ptrArg = arg;
                if (!ptrArg->getType()->isPointerTy()) {
                    const auto *P2I = dyn_cast<PtrToIntInst>(arg);
                    if (!P2I) continue;
                    ptrArg = P2I->getPointerOperand();
                }
                SharedAccess sa;
                sa.kind   = SharedAccess::Kind::Store; sa.irInst = "";
                sa.ptrVal = ptrArg;
                if (ptrArg->hasName()) sa.ptrDesc = ("@" + ptrArg->getName()).str();
                else { std::string t; raw_string_ostream o(t); ptrArg->printAsOperand(o, false); sa.ptrDesc = t; }
                return sa;
            }
        }
        return std::nullopt;
    };

    // ── 5. For each anchored function: find the anchor instruction, bracket ─
    for (const auto &[fnName, anchorHits] : anchors.perFunction) {
        Function *F = M.getFunction(fnName);
        if (!F || F->isDeclaration())
            continue;

        // Flatten the function into a linear list of instructions in BB order.
        std::vector<Instruction *> insts;
        for (BasicBlock &BB : *F)
            for (Instruction &I : BB)
                insts.push_back(&I);

        // Locate "anchor instructions" — inline asm calls and known ext calls.
        // An anchor site is the instruction index where the anchor occurs.
        // Multiple AnchorHits may map to the same instruction (e.g. a single
        // asm block has both dmb and stlr); we key brackets by AnchorHit index.

        // Map instruction → which anchor hits it represents.
        // We identify an anchor instruction as:
        //   (a) A CallInst/CallBrInst with an InlineAsm callee, OR
        //   (b) A CallInst calling a known external anchor function.
        // For simplicity we pair each AnchorHit with the instruction that
        // best represents it by matching site text.
        auto isAnchorInst = [&](const Instruction *I,
                                 const AnchorHit &hit) -> bool {
            // Inline asm hit — site = "asm sideeffect"
            if (hit.site == "asm sideeffect") {
                // Accept CallInst or CallBrInst with InlineAsm callee.
                // Also require the asm string to contain the hit label (mnemonic)
                // to avoid matching unrelated inline asm (e.g. CPU-feature probes).
                const InlineAsm *IA = nullptr;
                if (const auto *CI = dyn_cast<CallInst>(I))
                    IA = dyn_cast<InlineAsm>(CI->getCalledOperand());
                else if (const auto *CBR = dyn_cast<CallBrInst>(I))
                    IA = dyn_cast<InlineAsm>(CBR->getCalledOperand());
                if (!IA) return false;
                // Match: the asm string must contain the hit's label.
                return IA->getAsmString().find(hit.label) != std::string::npos;
            }
            // External call hit — site = "@funcName"
            if (hit.site.size() > 1 && hit.site[0] == '@') {
                std::string callee = hit.site.substr(1);
                if (const auto *CI = dyn_cast<CallInst>(I)) {
                    if (Function *fn = CI->getCalledFunction())
                        return fn->getName() == callee;
                }
                return false;
            }
            return false;
        };

        std::vector<AnchorBracket> brackets;

        for (const AnchorHit &hit : anchorHits) {
            // Find the first instruction in the flattened list that matches
            // this hit.
            int anchorIdx = -1;
            for (int i = 0; i < (int)insts.size(); ++i) {
                if (isAnchorInst(insts[i], hit)) {
                    anchorIdx = i;
                    break;
                }
            }
            if (anchorIdx < 0)
                continue; // anchor instruction not found in this function

            AnchorBracket bracket;
            bracket.anchor      = hit;
            bracket.anchorAlloc = findAnchorAlloc(insts[anchorIdx]);

            // Scan backwards for the last shared load/store before the anchor.
            for (int i = anchorIdx - 1; i >= 0; --i) {
                const Instruction *I = insts[i];
                if (const auto *LI = dyn_cast<LoadInst>(I)) {
                    if (touchesShared(LI->getPointerOperand())) {
                        bracket.before = makeAccess(*I, LI->getPointerOperand(),
                                                    SharedAccess::Kind::Load);
                        break;
                    }
                } else if (const auto *SI = dyn_cast<StoreInst>(I)) {
                    if (touchesShared(SI->getPointerOperand())) {
                        bracket.before = makeAccess(*I, SI->getPointerOperand(),
                                                    SharedAccess::Kind::Store);
                        break;
                    }
                }
            }

            // Scan forwards for the first shared load/store after the anchor.
            for (int i = anchorIdx + 1; i < (int)insts.size(); ++i) {
                const Instruction *I = insts[i];
                if (const auto *LI = dyn_cast<LoadInst>(I)) {
                    if (touchesShared(LI->getPointerOperand())) {
                        bracket.after = makeAccess(*I, LI->getPointerOperand(),
                                                   SharedAccess::Kind::Load);
                        break;
                    }
                } else if (const auto *SI = dyn_cast<StoreInst>(I)) {
                    if (touchesShared(SI->getPointerOperand())) {
                        bracket.after = makeAccess(*I, SI->getPointerOperand(),
                                                   SharedAccess::Kind::Store);
                        break;
                    }
                }
            }

            // Only record brackets that have at least one shared access side.
            if (bracket.before || bracket.after)
                brackets.push_back(std::move(bracket));
        }

        // Deduplicate brackets: if two brackets have identical before/after IR
        // instructions they represent the same logical event (e.g. the compiler
        // emits both `casal` and a paired `dmb ish` for each cmpxchg). Keep
        // only the one with the strongest ordering; preserve insertion order.
        {
            std::vector<AnchorBracket> deduped;
            for (auto &br : brackets) {
                const std::string bKey = br.before ? br.before->irInst : "";
                const std::string aKey = br.after  ? br.after->irInst  : "";
                bool merged = false;
                for (auto &ex : deduped) {
                    const std::string ebKey = ex.before ? ex.before->irInst : "";
                    const std::string eaKey = ex.after  ? ex.after->irInst  : "";
                    if (ebKey == bKey && eaKey == aKey) {
                        if (br.anchor.ordering > ex.anchor.ordering)
                            ex = br; // replace with stronger anchor
                        merged = true;
                        break;
                    }
                }
                if (!merged)
                    deduped.push_back(std::move(br));
            }
            brackets = std::move(deduped);
        }

        if (!brackets.empty())
            result.perFunction[fnName] = std::move(brackets);
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    NodeIDAllocator::unset();

    return result;
}
