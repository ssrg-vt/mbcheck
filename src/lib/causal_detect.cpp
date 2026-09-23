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
// --strict-publish flag state (Action 2)
// ---------------------------------------------------------------------------
static bool g_strictPublish = false;
void setStrictPublish(bool on) { g_strictPublish = on; }
bool strictPublish()           { return g_strictPublish; }

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
    mbtime::Scope _tBrackets("bracket_extraction");
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

    // ── 6. Strict-publish synthesis (Action 2 — TP detection in real kernel)
    //
    // When --strict-publish is on, scan every defined function for the
    // canonical message-passing (MP) bug shape:
    //
    //     store payload, ptr @P        ; plain store, no STLR, no barrier
    //     store flag,    ptr @F        ; plain store of publication flag
    //
    // A pair (S_payload, S_flag) is reported as a synthetic AnchorBracket
    // representing a "missing release barrier" publish.  The bracket flows
    // through function_pairs and herd7_transpiler exactly like a real anchor
    // bracket; herd7_transpiler.emitAnchor specialises on hit.site=="store
    // plain" to emit a plain STR (no STLR, no DMB).
    //
    // Filters applied to keep noise down:
    //   - Function must have NO release-or-stronger anchor anywhere in its
    //     body on any global (it is genuinely not synchronising via the
    //     kernel APIs we recognise).
    //   - Two stores must be to DIFFERENT shared objects (single-object
    //     stores don't form an MP pattern).
    //   - Both store targets must be GLOBAL (heap-only stores are scoped to
    //     one allocation and not visible to a partner via name).
    //   - We pick the FIRST shared-object store as payload and the FIRST
    //     subsequent shared-object store to a DIFFERENT object as flag.
    //   - We synthesise at most ONE bracket per function (the first MP
    //     pair found) — keeps the candidate set bounded.
    if (g_strictPublish) {
        // Helper: given an LLVM Value*, return the first shared base
        // GlobalVariable* it points to (or nullptr).  Used to identify
        // the publication FLAG, which must be a named global so the partner
        // consumer can pattern-match the same channel.
        auto firstSharedGlobal = [&](const Value *ptrVal) -> const GlobalVariable * {
            if (!msSet->hasValueNode(ptrVal)) return nullptr;
            NodeID nid = msSet->getValueNode(ptrVal);
            const PointsTo &pts = pta->getPts(nid);
            for (NodeID obj : pts) {
                NodeID base = obj;
                if (const auto *g = dyn_cast<GepObjVar>(pag->getGNode(obj)))
                    base = g->getBaseObj()->getId();
                if (!sharedObjIds.count(base)) continue;
                SVFVar *v = pag->getGNode(base);
                if (v && msSet->hasLLVMValue(v)) {
                    if (const auto *GV =
                            dyn_cast<GlobalVariable>(msSet->getLLVMValue(v)))
                        return GV;
                }
            }
            return nullptr;
        };

        // Helper: opaque "shared object id" for a store target.  Used so the
        // payload and flag are required to refer to DIFFERENT shared objects.
        // We use the base PAG NodeID; heap-allocated payloads return the
        // heap NodeID, globals return the global NodeID.
        auto firstSharedBaseId = [&](const Value *ptrVal) -> NodeID {
            if (!msSet->hasValueNode(ptrVal)) return 0;
            NodeID nid = msSet->getValueNode(ptrVal);
            const PointsTo &pts = pta->getPts(nid);
            for (NodeID obj : pts) {
                NodeID base = obj;
                if (const auto *g = dyn_cast<GepObjVar>(pag->getGNode(obj)))
                    base = g->getBaseObj()->getId();
                if (sharedObjIds.count(base))
                    return base;
            }
            return 0;
        };

        for (Function &F : M) {
            if (F.isDeclaration())
                continue;
            const std::string fname = F.getName().str();

            // Skip if function already contains any inline-asm release-or-
            // stronger anchor (STLR, DMB ST, smp_store_release, …).  External
            // SC functions like __kmalloc_cache_noprof or flush_work are
            // incidental side effects, NOT synchronisation primitives that
            // pair the payload to the flag — leave them eligible for
            // strict-publish synthesis.
            auto aIt = anchors.perFunction.find(fname);
            if (aIt != anchors.perFunction.end()) {
                bool hasInlineAsmRelOrStronger = false;
                for (const AnchorHit &h : aIt->second) {
                    if (h.site == "asm sideeffect" &&
                        h.ordering >= anchors::Ordering::Release) {
                        hasInlineAsmRelOrStronger = true;
                        break;
                    }
                }
                if (hasInlineAsmRelOrStronger)
                    continue;
            }

            // Walk instructions, collect plain / volatile (WRITE_ONCE) shared
            // stores in program order.  Atomic stores are excluded since they
            // already have ordering semantics modelled elsewhere.
            //   - payload:  any shared object (heap or global)
            //   - flag:     must be a shared GLOBAL (so partner can match by name)
            struct PStore {
                const StoreInst *si;
                NodeID           baseId;       // 0 if not shared
                const GlobalVariable *flagGV;  // non-null iff this store targets a shared global
            };
            std::vector<PStore> pstores;
            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {
                    auto *SI = dyn_cast<StoreInst>(&I);
                    if (!SI) continue;
                    if (SI->isAtomic()) continue;   // already ordered
                    NodeID b = firstSharedBaseId(SI->getPointerOperand());
                    if (!b) continue;
                    const GlobalVariable *GV =
                        firstSharedGlobal(SI->getPointerOperand());
                    pstores.push_back({SI, b, GV});
                }
            }
            if (pstores.size() < 2)
                continue;

            // Find first (payload, flag) pair:
            //   - flag must be a shared global (flagGV != nullptr)
            //   - payload must precede flag in program order
            //   - payload.baseId != flag.baseId (distinct shared objects)
            const StoreInst *payloadSI = nullptr;
            const StoreInst *flagSI    = nullptr;
            for (size_t j = 1; j < pstores.size() && !flagSI; ++j) {
                if (!pstores[j].flagGV) continue;
                for (size_t i = 0; i < j; ++i) {
                    if (pstores[i].baseId == pstores[j].baseId) continue;
                    payloadSI = pstores[i].si;
                    flagSI    = pstores[j].si;
                    break;
                }
            }
            if (!payloadSI || !flagSI)
                continue;

            // Build the synthetic bracket.
            AnchorBracket bracket;
            bracket.anchor.label    = "store (plain, no barrier)";
            bracket.anchor.site     = "store plain";
            bracket.anchor.ordering = anchors::Ordering::Relaxed;
            bracket.anchor.asmInst  = "";
            bracket.anchorAlloc =
                makeAccess(*flagSI, flagSI->getPointerOperand(),
                           SharedAccess::Kind::Store);
            bracket.before =
                makeAccess(*payloadSI, payloadSI->getPointerOperand(),
                           SharedAccess::Kind::Store);
            // bracket.after intentionally empty — the flag store IS the
            // publication; nothing after it is relevant for the MP test.

            result.perFunction[fname].push_back(std::move(bracket));
        }
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    NodeIDAllocator::unset();

    return result;
}
