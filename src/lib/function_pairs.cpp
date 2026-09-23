#include "lib/phase_timer.h"
#include "lib/function_pairs.h"
#include "lib/causal_detect.h"
#include "lib/allocatorPre.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Options.h"
#include "SVFIR/SVFIR.h"
#include "Util/NodeIDAllocator.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>

using namespace llvm;
using namespace SVF;

AnalysisKey FunctionPairsPass::Key;

// ---------------------------------------------------------------------------
// Helper: render an instruction as a compact one-line string (≤ 120 chars)
// ---------------------------------------------------------------------------
static std::string instToStr(const Instruction &I)
{
    std::string s;
    raw_string_ostream os(s);
    I.print(os);
    size_t start = s.find_first_not_of(" \t");
    if (start != std::string::npos)
        s = s.substr(start);
    size_t nl = s.find('\n');
    if (nl != std::string::npos)
        s = s.substr(0, nl);
    if (s.size() > 120)
        s = s.substr(0, 117) + "...";
    return s;
}

// ---------------------------------------------------------------------------
// Pass implementation
// ---------------------------------------------------------------------------
FunctionPairsResult FunctionPairsPass::run(Module &M,
                                            ModuleAnalysisManager &MAM)
{
    mbtime::Scope _tPairing("function_pairing");
    FunctionPairsResult result;

    // ── 1. Obtain CausalDetect results ──────────────────────────────────────
    // This runs CausalDetectPass (which internally builds + tears down SVF).
    const CausalDetectResult &causal = MAM.getResult<CausalDetectPass>(M);
    if (causal.perFunction.empty())
        return result;

    // ── 2. Rebuild SVF Andersen PTA ─────────────────────────────────────────
    // CausalDetectPass tears down SVF at the end of its run, so we rebuild
    // an identical analysis context here.
    const_cast<Option<bool> &>(Options::PStat).setValue(false);
    normaliseKernelAllocators(M); // idempotent: already-replaced allocs are no-ops

    LLVMModuleSet::buildSVFModule(M);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();
    auto *pta  = AndersenWaveDiff::createAndersenWaveDiff(pag);
    auto *msSet = LLVMModuleSet::getLLVMModuleSet();

    // ── 3. Compute shared object set ────────────────────────────────────────
    // Mirrors the logic in CausalDetectPass::run() exactly.
    //
    // T1.2: extend reachability to multi-level ConstantExpr chains.  A global
    // wrapped in nested CEs (e.g. `bitcast (gep (bitcast @g, ...))`) was
    // previously missed because the original walk only descended one CE level.
    // We now do a transitive visited-set walk so any global ultimately
    // referenced from instruction context counts as "used".
    DenseSet<const Value *> usedGlobals;
    {
        std::function<bool(const User *, DenseSet<const User *> &)> reachesInst;
        reachesInst = [&](const User *U,
                          DenseSet<const User *> &seen) -> bool {
            if (!seen.insert(U).second) return false;
            if (isa<Instruction>(U)) return true;
            if (isa<ConstantExpr>(U) || isa<ConstantAggregate>(U)) {
                for (const auto *UU : U->users())
                    if (reachesInst(UU, seen)) return true;
            }
            return false;
        };
        for (const auto &GV : M.globals()) {
            DenseSet<const User *> seen;
            for (const auto *U : GV.users())
                if (reachesInst(U, seen)) {
                    usedGlobals.insert(&GV);
                    break;
                }
        }
    }

    DenseSet<NodeID> sharedObjIds;
    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it) {
        NodeID  nid    = it->first;
        SVFVar *svfVar = it->second;
        if (!isa<ObjVar>(svfVar))
            continue;
        bool isHeap   = isa<HeapObjVar>(svfVar);
        bool isGlobal = isa<GlobalObjVar>(svfVar);
        if (!isHeap && !isGlobal)
            continue;
        if (isGlobal) {
            if (!msSet->hasLLVMValue(svfVar))
                continue;
            const Value *llvmVal = msSet->getLLVMValue(svfVar);
            if (!usedGlobals.count(llvmVal))
                continue;
        }
        sharedObjIds.insert(nid);
    }

    // ── 4. Helper: base-object IDs reachable from an LLVM pointer value ─────
    // Returns the set of shared-base NodeIDs that `V` points to.  GEP fields
    // are normalised to their allocation's base object so that accesses to
    // different fields of the same allocation match each other.
    auto getBaseObjIds = [&](const Value *V) -> DenseSet<NodeID> {
        DenseSet<NodeID> ids;
        if (!V || !msSet->hasValueNode(V))
            return ids;
        NodeID nid = msSet->getValueNode(V);
        const PointsTo &pts = pta->getPts(nid);
        for (NodeID obj : pts) {
            if (sharedObjIds.count(obj)) {
                ids.insert(obj);
            } else if (const auto *gep =
                           dyn_cast<GepObjVar>(pag->getGNode(obj))) {
                NodeID base = gep->getBaseObj()->getId();
                if (sharedObjIds.count(base))
                    ids.insert(base);
            }
        }
        return ids;
    };

    // Helper: human-readable name for a base object NodeID.
    auto objName = [&](NodeID id) -> std::string {
        SVFVar *v = pag->getGNode(id);
        if (v && msSet->hasLLVMValue(v)) {
            const Value *lv = msSet->getLLVMValue(v);
            if (lv->hasName())
                return "@" + lv->getName().str();
        }
        return "(obj:" + std::to_string(id) + ")";
    };

    // ── 5. Build per-function footprint ─────────────────────────────────────
    // For every defined function collect the set of base-object NodeIDs that
    // it accesses through any of:
    //   - explicit LLVM load/store (pointer operand)
    //   - inline-asm CallInst/CallBrInst pointer-typed arguments (covers
    //     smp_load_acquire / smp_store_release which lower to "ldar"/"stlr"
    //     inline asm and never appear as plain LLVM load/store)
    //   - external-function call pointer-typed arguments (spin_lock etc.)
    auto addPtrArgsToFootprint = [&](DenseSet<NodeID> &fp,
                                     const Value *arg) {
        const Value *ptrArg = arg;
        if (!ptrArg->getType()->isPointerTy()) {
            const auto *P2I = dyn_cast<PtrToIntInst>(arg);
            if (!P2I) return;
            ptrArg = P2I->getPointerOperand();
        }
        for (NodeID id : getBaseObjIds(ptrArg))
            fp.insert(id);
    };

    std::map<std::string, DenseSet<NodeID>> footprint;
    // T#4 — collect in-module direct callee/caller relation while scanning,
    // so we can propagate footprints transitively below.
    std::map<std::string, std::set<std::string>> calleeMap;
    std::map<std::string, std::set<std::string>> callerMap;
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        const std::string fname = F.getName().str();
        auto &fp = footprint[fname];
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    for (NodeID id : getBaseObjIds(LI->getPointerOperand()))
                        fp.insert(id);
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    for (NodeID id : getBaseObjIds(SI->getPointerOperand()))
                        fp.insert(id);
                } else if (auto *CI = dyn_cast<CallInst>(&I)) {
                    for (unsigned i = 0; i < CI->arg_size(); ++i)
                        addPtrArgsToFootprint(fp, CI->getArgOperand(i));
                    if (const Function *callee = CI->getCalledFunction())
                        if (!callee->isDeclaration()) {
                            const std::string cn = callee->getName().str();
                            calleeMap[fname].insert(cn);
                            callerMap[cn].insert(fname);
                        }
                } else if (auto *CBR = dyn_cast<CallBrInst>(&I)) {
                    for (unsigned i = 0; i < CBR->arg_size(); ++i)
                        addPtrArgsToFootprint(fp, CBR->getArgOperand(i));
                    if (const Function *callee = CBR->getCalledFunction())
                        if (!callee->isDeclaration()) {
                            const std::string cn = callee->getName().str();
                            calleeMap[fname].insert(cn);
                            callerMap[cn].insert(fname);
                        }
                }
            }
        }
    }

    // ── 5b. T#4 — Callee-transitive footprint propagation ───────────────────
    // A function's footprint is augmented with its callees' footprints so
    // that pairs whose shared access lives in a static helper are still
    // matched.  Worklist fixpoint over the in-module direct call graph.
    // Indirect calls are not resolved here (out of scope).
    {
        std::vector<std::string> worklist;
        worklist.reserve(footprint.size());
        for (auto &kv : footprint) worklist.push_back(kv.first);
        while (!worklist.empty()) {
            const std::string f = std::move(worklist.back());
            worklist.pop_back();
            auto cit = calleeMap.find(f);
            if (cit == calleeMap.end()) continue;
            auto &ffp = footprint[f];
            bool changed = false;
            for (const std::string &c : cit->second) {
                auto gIt = footprint.find(c);
                if (gIt == footprint.end()) continue;
                for (NodeID id : gIt->second) {
                    if (ffp.insert(id).second) changed = true;
                }
            }
            if (changed) {
                auto rIt = callerMap.find(f);
                if (rIt != callerMap.end())
                    for (const std::string &p : rIt->second)
                        worklist.push_back(p);
            }
        }
    }

    // ── 6. Match anchor brackets to partner functions ───────────────────────
    for (const auto &[fnName, brackets] : causal.perFunction) {
        for (const AnchorBracket &br : brackets) {
            
            // Determine the sets of objects this bracket "requires" a partner
            // to access.

            // channel_ids: objects the anchor itself operates on (the
            //   "publication channel", e.g. the NodeID of g_ptr for stlr).
            DenseSet<NodeID> channelIds;
            if (br.anchorAlloc && br.anchorAlloc->ptrVal)
                channelIds = getBaseObjIds(br.anchorAlloc->ptrVal);

            // causal_ids: objects accessed immediately before/after the anchor
            //   (the "payload", e.g. the heap object for msg->value).
            DenseSet<NodeID> causalIds;
            if (br.before && br.before->ptrVal)
                for (NodeID id : getBaseObjIds(br.before->ptrVal))
                    causalIds.insert(id);
            if (br.after && br.after->ptrVal)
                for (NodeID id : getBaseObjIds(br.after->ptrVal))
                    causalIds.insert(id);

            bool hasChannel = !channelIds.empty();

            // For ThreadBarriers without alloc choose the "channel" to be the
            // object on the ordering-critical side of the barrier:
            //   Release / SC  → use the AFTER object (the flag/pointer that
            //                   is published after the barrier)
            //   Acquire       → use the BEFORE object (the flag/pointer that
            //                   is polled before the barrier)
            // Fallback: whichever side has a non-empty object set.
            DenseSet<NodeID> barrierChannelIds;
            if (!hasChannel) {
                DenseSet<NodeID> beforeObjs, afterObjs;
                if (br.before && br.before->ptrVal)
                    beforeObjs = getBaseObjIds(br.before->ptrVal);
                if (br.after && br.after->ptrVal)
                    afterObjs = getBaseObjIds(br.after->ptrVal);

                using O = anchors::Ordering;
                if (br.anchor.ordering >= O::Release && !afterObjs.empty())
                    barrierChannelIds = afterObjs;
                else if (br.anchor.ordering == O::Acquire && !beforeObjs.empty())
                    barrierChannelIds = beforeObjs;
                else if (!afterObjs.empty())
                    barrierChannelIds = afterObjs;
                else if (!beforeObjs.empty())
                    barrierChannelIds = beforeObjs;

                if (barrierChannelIds.empty())
                    continue; // nothing to match on
            }

            // Scan all non-anchor functions for a matching footprint.
            for (auto &[gName, gFp] : footprint) {
                if (gName == fnName)
                    continue;
                if (gFp.empty())
                    continue;

                bool matched = false;

                if (hasChannel) {
                    // Case A: anchor with alloc (Store / RMW / ext).
                    // Partner must access the channel object.  We do NOT
                    // require causal-object access here because kernel code
                    // often reads the payload through an inttoptr chain
                    // (inline-asm returns i64, then inttoptr) which SVF PTA
                    // cannot track.  Channel-only matching is sufficient to
                    // identify the paired function.
                    for (NodeID id : channelIds)
                        if (gFp.count(id)) { matched = true; break; }
                } else {
                    // Case B: ThreadBarrier (dmb/dsb — no alloc).
                    // Partner must access the barrier's "channel" side
                    // (release → after-object; acquire → before-object).
                    for (NodeID id : barrierChannelIds)
                        if (gFp.count(id)) { matched = true; break; }
                }

                if (!matched)
                    continue;

                // Deduplicate: don't add the same (partnerFn, anchor.label)
                // pair for the same anchor function twice.
                auto &pairs = result.byAnchorFn[fnName];
                bool dup = false;
                for (const auto &p : pairs)
                    if (p.partnerFn == gName &&
                        p.bracket.anchor.label == br.anchor.label) {
                        dup = true;
                        break;
                    }
                if (!dup) {
                    FunctionPair pair;
                    pair.anchorFn  = fnName;
                    pair.partnerFn = gName;
                    pair.bracket   = br;

                    // ── Collect partner accesses ─────────────────────────
                    // Scan partnerFn for every load/store/call that touches
                    // the channel or causal shared objects from this bracket.
                    Function *gFn = M.getFunction(gName);
                    if (gFn && !gFn->isDeclaration()) {
                        // Build the set of objects to look for in partnerFn.
                        DenseSet<NodeID> relevantIds;
                        if (hasChannel) {
                            for (NodeID id : channelIds) relevantIds.insert(id);
                            for (NodeID id : causalIds)  relevantIds.insert(id);
                        } else {
                            // Barrier: include both before and after objects.
                            if (br.before && br.before->ptrVal)
                                for (NodeID id : getBaseObjIds(br.before->ptrVal))
                                    relevantIds.insert(id);
                            if (br.after && br.after->ptrVal)
                                for (NodeID id : getBaseObjIds(br.after->ptrVal))
                                    relevantIds.insert(id);
                        }

                        // Returns the first relevant base-object NodeID that
                        // value V touches (via PTA), or ~0u if none.
                        // Also handles the inttoptr chain produced by inline-asm
                        // loads (e.g. "ldar" returns i64, then inttoptr converts
                        // it to ptr) where SVF PTA cannot propagate points-to
                        // information through the i64 integer cast.
                        auto hitsRelevant = [&](const Value *V) -> NodeID {
                            const Value *ptrV = V;
                            if (!ptrV->getType()->isPointerTy()) {
                                const auto *P2I = dyn_cast<PtrToIntInst>(V);
                                if (!P2I) return ~0u;
                                ptrV = P2I->getPointerOperand();
                            }
                            // Standard SVF PTA check.
                            if (msSet->hasValueNode(ptrV)) {
                                NodeID nid = msSet->getValueNode(ptrV);
                                const PointsTo &pts = pta->getPts(nid);
                                for (NodeID obj : pts) {
                                    if (relevantIds.count(obj)) return obj;
                                    if (const auto *gep =
                                            dyn_cast<GepObjVar>(pag->getGNode(obj)))
                                        if (relevantIds.count(
                                                gep->getBaseObj()->getId()))
                                            return gep->getBaseObj()->getId();
                                }
                            }
                            // Inttoptr chain: kernel inline-asm loads (ldar /
                            // smp_load_acquire) return the loaded address as i64;
                            // the caller then converts it back with inttoptr.
                            // SVF cannot track points-to through i64, so we detect
                            // this pattern explicitly:
                            //   %1 = call i64 asm "ldar ..." (ptr @channel)
                            //   %4 = inttoptr i64 %1 to ptr    ← ptrV here
                            //   load ..., ptr %4               ← caller of hitsRelevant
                            // We also handle a single GEP on top of the inttoptr:
                            //   %6 = gep ..., ptr %4           ← ptrV here
                            //   call f(ptr %6)
                            // For alloc anchors, the effective channel is channelIds.
                            // For ThreadBarrier anchors (hasChannel==false),
                            // barrierChannelIds plays the same role.
                            const DenseSet<NodeID> &effectiveChannelIds =
                                hasChannel ? channelIds : barrierChannelIds;
                            // Helper: is this Value an SVF-known pointer that
                            // points to (or aliases) a channel object?
                            auto svfHitsChannel = [&](const Value *p) -> bool {
                                if (!p || !p->getType()->isPointerTy())
                                    return false;
                                if (!msSet->hasValueNode(p)) return false;
                                NodeID pNid = msSet->getValueNode(p);
                                const PointsTo &pts = pta->getPts(pNid);
                                for (NodeID obj : pts) {
                                    NodeID b = obj;
                                    if (const auto *g = dyn_cast<GepObjVar>(
                                            pag->getGNode(obj)))
                                        b = g->getBaseObj()->getId();
                                    if (effectiveChannelIds.count(b))
                                        return true;
                                }
                                return false;
                            };
                            // Helper: producer publishes the payload via inline-asm
                            // STLR (smp_store_release / cmpxchg_release / xchg /
                            // ...).  SVF cannot see those stores, so pts(@channel)
                            // has no payload object.  When ptrV is derived from a
                            // *plain* load of @channel (consumer-side bug pattern:
                            // missing READ_ONCE / smp_load_acquire), we infer that
                            // the dereference targets the payload causal object.
                            // Returns the causal/relevant payload id, or ~0u.
                            auto attributeChannelDerived = [&]() -> NodeID {
                                if (hasChannel && !causalIds.empty())
                                    return *causalIds.begin();
                                for (NodeID rid : relevantIds)
                                    if (!effectiveChannelIds.count(rid))
                                        return rid;
                                return ~0u;
                            };
                            if (!effectiveChannelIds.empty()) {
                                const Value *base = ptrV;
                                if (const auto *GEP =
                                        dyn_cast<GetElementPtrInst>(base))
                                    base = GEP->getPointerOperand();
                                if (const auto *I2P =
                                        dyn_cast<IntToPtrInst>(base)) {
                                    const auto *srcCI =
                                        dyn_cast<CallInst>(I2P->getOperand(0));
                                    if (srcCI && srcCI->isInlineAsm()) {
                                        for (unsigned i = 0;
                                             i < srcCI->arg_size(); ++i) {
                                            const Value *arg =
                                                srcCI->getArgOperand(i);
                                            if (!arg->getType()->isPointerTy())
                                                continue;
                                            if (!msSet->hasValueNode(arg))
                                                continue;
                                            NodeID argNid =
                                                msSet->getValueNode(arg);
                                            const PointsTo &argPts =
                                                pta->getPts(argNid);
                                            for (NodeID obj : argPts) {
                                                NodeID b = obj;
                                                if (const auto *g =
                                                        dyn_cast<GepObjVar>(
                                                            pag->getGNode(obj)))
                                                    b = g->getBaseObj()->getId();
                                                if (effectiveChannelIds.count(b) ||
                                                    relevantIds.count(b)) {
                                                    NodeID r =
                                                        attributeChannelDerived();
                                                    if (r != ~0u) return r;
                                                    return b;
                                                }
                                            }
                                        }
                                    }
                                }
                                // Plain-load chain: kernel writes the channel via
                                // inline-asm STLR (smp_store_release) which SVF
                                // cannot track; consumer reads with a plain LLVM
                                // `load ptr, ptr @channel` (the "bug" pattern: no
                                // READ_ONCE / smp_load_acquire).  We then see:
                                //   %p = load ptr, ptr @channel       (channel ld)
                                //   %v = load i32, ptr %p              ← ptrV=%p
                                //   call f(ptr %p)                     ← ptrV=%p
                                //   %g = gep ..., ptr %p; load ..., %g ← ptrV=%g
                                // Detect: walk back through GEPs to the LoadInst,
                                // then check if the load's address-operand points
                                // to a channel object via SVF.
                                const Value *p = ptrV;
                                if (const auto *GEP =
                                        dyn_cast<GetElementPtrInst>(p))
                                    p = GEP->getPointerOperand();
                                if (const auto *LD = dyn_cast<LoadInst>(p)) {
                                    if (svfHitsChannel(LD->getPointerOperand())) {
                                        NodeID r = attributeChannelDerived();
                                        if (r != ~0u) return r;
                                    }
                                }
                            }
                            return ~0u;
                        };

                        // Emit the first instruction per shared object.
                        // Subsequent instructions touching the same NodeID
                        // are skipped — one representative access is enough.
                        DenseSet<NodeID> seenObjs;
                        for (BasicBlock &BB : *gFn) {
                            for (Instruction &I : BB) {
                                NodeID hit = ~0u;
                                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                                    hit = hitsRelevant(LI->getPointerOperand());
                                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                                    hit = hitsRelevant(SI->getPointerOperand());
                                } else if (auto *CI = dyn_cast<CallInst>(&I)) {
                                    for (unsigned i = 0;
                                         i < CI->arg_size() && hit == ~0u; ++i)
                                        hit = hitsRelevant(CI->getArgOperand(i));
                                } else if (auto *CBR = dyn_cast<CallBrInst>(&I)) {
                                    for (unsigned i = 0;
                                         i < CBR->arg_size() && hit == ~0u; ++i)
                                        hit = hitsRelevant(CBR->getArgOperand(i));
                                }
                                if (hit == ~0u) continue;
                                if (!seenObjs.insert(hit).second) continue;
                                PartnerAccess pa;
                                pa.irInst  = instToStr(I);
                                pa.objDesc = objName(hit);
                                pair.partnerAccesses.push_back(std::move(pa));
                            }
                        }
                    }

                    pairs.push_back(std::move(pair));
                }
            }
        }
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    NodeIDAllocator::unset();

    return result;
}
