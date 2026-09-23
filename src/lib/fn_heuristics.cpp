#include "lib/phase_timer.h"
#include "lib/fn_heuristics.h"
#include "lib/function_pairs.h"
#include "lib/allocatorPre.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Options.h"
#include "SVFIR/SVFIR.h"
#include "Util/NodeIDAllocator.h"

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <queue>
#include <set>
#include <unordered_set>
#include <functional>

using namespace llvm;
using namespace SVF;

// ---------------------------------------------------------------------------
// Ablation mode storage (T2.1)
// ---------------------------------------------------------------------------
static std::set<std::string> &mutableDisabledHeuristics() {
    static std::set<std::string> g_disabled;
    return g_disabled;
}
void setDisabledHeuristics(std::set<std::string> names) {
    mutableDisabledHeuristics() = std::move(names);
}
const std::set<std::string> &disabledHeuristics() {
    return mutableDisabledHeuristics();
}

AnalysisKey FnHeuristicsFilterPass::Key;

// ---------------------------------------------------------------------------
// Internal helper: BFS reachability in the LLVM call graph.
//
// Returns true if `to` is reachable from `from` by following call edges
// (direct or transitive).  The external-call node (index ~0u, representing
// "called from outside the module") is deliberately skipped so we only
// consider in-module call chains.
// ---------------------------------------------------------------------------
static bool cgReachable(const llvm::CallGraph &CG,
                        const Function  *from,
                        const Function  *to)
{
    if (from == to)
        return true;

    const llvm::CallGraphNode *start = CG[from];
    if (!start)
        return false;

    std::unordered_set<const llvm::CallGraphNode *> visited;
    std::queue<const llvm::CallGraphNode *>         worklist;

    worklist.push(start);
    visited.insert(start);

    while (!worklist.empty()) {
        const llvm::CallGraphNode *node = worklist.front();
        worklist.pop();

        for (const auto &edge : *node) {
            const llvm::CallGraphNode *callee = edge.second;
            if (!callee)
                continue;
            if (!callee->getFunction())
                continue;
            if (callee->getFunction() == to)
                return true;
            if (visited.insert(callee).second)
                worklist.push(callee);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ReadReadPair::isMutuallyExclusive  (H_RR)
//
// Sound: a data race requires at least one write.  If every access on both
// sides is a load, the pair cannot be a race.
// ---------------------------------------------------------------------------

// Returns true when an IR instruction string represents a store operation.
// Covers plain LLVM stores ("store ") and inline-asm release stores (stlr,
// swpal).  Read-modify-write atomics (ldaddal, ldsetal, ldclral) also write,
// but they will have Kind::Store in the AnchorBracket, so they are caught by
// the bracket-side check instead.
static bool irInstIsStore(const std::string &ir) {
    if (ir.size() >= 6 && ir.compare(0, 6, "store ") == 0)
        return true;
    if (ir.find("stlr")  != std::string::npos) return true;
    if (ir.find("swpal") != std::string::npos) return true;
    return false;
}

std::string ReadReadPair::isMutuallyExclusive(
        const FunctionPair &p,
        llvm::Module & /*M*/,
        llvm::ModuleAnalysisManager & /*MAM*/) const
{
    const AnchorBracket &br = p.bracket;

    // Anchor bracket: check before/after for stores.
    if (br.before && br.before->kind == SharedAccess::Kind::Store)
        return "";
    if (br.after  && br.after->kind  == SharedAccess::Kind::Store)
        return "";

    // Partner accesses: check each IR string.
    for (const PartnerAccess &pa : p.partnerAccesses)
        if (irInstIsStore(pa.irInst))
            return "";

    // All loads on both sides — cannot be a data race.
    return "all-loads";
}

// ---------------------------------------------------------------------------
// CallGraphHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string CallGraphHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  &M,
    ModuleAnalysisManager   &MAM) const
{
    const llvm::CallGraph &CG = MAM.getResult<CallGraphAnalysis>(M);

    Function *fA = M.getFunction(pair.anchorFn);
    Function *fB = M.getFunction(pair.partnerFn);
    if (!fA || !fB)
        return "";

    // G5.2 IRQ-context correction: if exactly one of (anchorFn, partnerFn)
    // is reachable from an IRQ-handler / tasklet / work-queue entry point
    // and the other is not, the two functions run in DIFFERENT execution
    // contexts even if statically call-graph-reachable.  The IRQ handler
    // can preempt or run concurrently with the process-context caller on a
    // different CPU.  Skip the suppression — the pair is genuinely racy.
    bool aIrq = irqReachable_.count(pair.anchorFn)  > 0;
    bool bIrq = irqReachable_.count(pair.partnerFn) > 0;
    if (aIrq != bIrq)
        return "";

    // Mutually exclusive if either function is transitively reachable from
    // the other: they run in the same call chain and cannot be concurrent.
    //
    // NOTE: The "common direct caller" check (fn1 and fn2 both called from
    // the same function) is intentionally omitted — it is unsound.  Sharing
    // one sequential call site does not prevent fn1/fn2 from being invoked
    // concurrently from other contexts (IRQ handlers, separate threads, etc.).
    if (cgReachable(CG, fA, fB))
        return pair.anchorFn + " calls " + pair.partnerFn;
    if (cgReachable(CG, fB, fA))
        return pair.partnerFn + " calls " + pair.anchorFn;
    return "";
}

// ---------------------------------------------------------------------------
// LockAliasHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string LockAliasHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    auto itA = lockSets_.find(pair.anchorFn);
    auto itB = lockSets_.find(pair.partnerFn);
    if (itA == lockSets_.end() || itB == lockSets_.end())
        return "";

    // Mutually exclusive if both functions acquire at least one common lock.
    for (unsigned id : itA->second) {
        if (itB->second.count(id)) {
            auto it = lockNames_.find(id);
            return (it != lockNames_.end()) ? it->second : "@lock:" + std::to_string(id);
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// LifecycleHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string LifecycleHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    bool aInit     = initOnly_.count(pair.anchorFn)        > 0;
    bool aExit     = exitOnly_.count(pair.anchorFn)        > 0;
    bool bInit     = initOnly_.count(pair.partnerFn)       > 0;
    bool bExit     = exitOnly_.count(pair.partnerFn)       > 0;
    bool aMdInit   = moduleInitOnly_.count(pair.anchorFn)  > 0;
    bool aMdExit   = moduleExitOnly_.count(pair.anchorFn)  > 0;
    bool bMdInit   = moduleInitOnly_.count(pair.partnerFn) > 0;
    bool bMdExit   = moduleExitOnly_.count(pair.partnerFn) > 0;

    // init-only and exit-only phases never overlap in the module lifecycle
    // (init completes before runtime starts; exit runs after runtime drains).
    if ((aInit && bExit) || (aExit && bInit)) return "init+exit";
    // Two init-only functions run sequentially (module init is single-threaded).
    if (aInit && bInit) return "init+init";
    // Two exit-only functions run sequentially (module exit is single-threaded).
    if (aExit && bExit) return "exit+exit";

    // H27: transitive module-init-only / module-exit-only filtering.
    if (aMdInit || bMdInit) return "module-init-only";
    if (aMdExit || bMdExit) return "module-exit-only";

    return "";
}

// ---------------------------------------------------------------------------
// RcuSerialHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string RcuSerialHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    bool aReader = rcuReaders_.count(pair.anchorFn)  > 0;
    bool aSync   = rcuSync_.count(pair.anchorFn)     > 0;
    bool bReader = rcuReaders_.count(pair.partnerFn) > 0;
    bool bSync   = rcuSync_.count(pair.partnerFn)    > 0;

    // RCU grace period strictly orders reader and synchronize_rcu caller.
    if ((aReader && bSync) || (aSync && bReader))
        return "rcu-reader+sync";
    return "";
}

// ---------------------------------------------------------------------------
// CompletionOrderHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string CompletionOrderHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    auto hasCommon = [](const std::set<unsigned> &a,
                        const std::set<unsigned> &b) -> bool {
        for (unsigned id : a)
            if (b.count(id)) return true;
        return false;
    };

    auto itSigA  = signal_.find(pair.anchorFn);
    auto itWaitB = wait_.find(pair.partnerFn);
    if (itSigA != signal_.end() && itWaitB != wait_.end())
        if (hasCommon(itSigA->second, itWaitB->second))
            return pair.anchorFn + "->complete + " + pair.partnerFn + "->wait";

    auto itSigB  = signal_.find(pair.partnerFn);
    auto itWaitA = wait_.find(pair.anchorFn);
    if (itSigB != signal_.end() && itWaitA != wait_.end())
        if (hasCommon(itSigB->second, itWaitA->second))
            return pair.partnerFn + "->complete + " + pair.anchorFn + "->wait";

    return "";
}

// Returns true if a callee name corresponds to an exclusive-lock acquire —
// i.e. a call that prevents any other thread from simultaneously acquiring
// the same lock.  Read-locks (_raw_read_lock) and RCU read-side markers
// (__rcu_read_lock) are excluded because multiple readers may run at once.
//
// Bit-spinlocks (folio_lock / lock_buffer / __bit_spin_lock) are included
// because they semantically behave as exclusive locks even though their
// "argument 0" identifies a (struct, bit) pair rather than a dedicated
// spinlock_t.  We add them so LockAliasHeuristic's PTA-seeded lockSets
// include the locked object's base ID; when two functions both lock the
// same bit on the same object, the heuristic correctly suppresses the pair.
static bool isExclusiveLockAcquire(const StringRef name)
{
    // Exact matches for common no-suffix forms.
    static const char *const exact[] = {
        // H2: raw spinlocks
        "_raw_spin_lock",
        "_raw_spin_lock_irq",
        "_raw_spin_lock_bh",
        "_raw_spin_lock_irqsave",
        "_raw_spin_lock_nested",
        "_raw_write_lock",
        "_raw_write_lock_irq",
        "_raw_write_lock_bh",
        "_raw_write_lock_irqsave",
        // H9: mutex
        "mutex_lock",
        "mutex_lock_interruptible",
        "mutex_lock_killable",
        "mutex_lock_nested",
        // H9: semaphore (down = mutex-like exclusive)
        "down",
        "down_interruptible",
        "down_killable",
        "down_trylock",
        "down_timeout",
        // H7: seqlock writer/writer exclusion
        "write_seqlock",
        "write_seqlock_irq",
        "write_seqlock_bh",
        "write_seqlock_irqsave",
        "_raw_write_seqlock",
        // H10: rwsem writer/writer exclusion
        "down_write",
        "down_write_interruptible",
        "down_write_killable",
        "down_write_trylock",
        // G1.4: bit-spinlocks — the "lock variable" is a bit in the first
        // argument's storage.  We treat the whole object as the lock; if
        // two functions lock the same folio/buffer, they are mutex.
        "folio_lock",
        "folio_lock_killable",
        "__folio_lock",
        "__folio_lock_killable",
        "lock_buffer",
        "__lock_buffer",
        "lock_page",
        "__lock_page",
        "lock_page_killable",
        "bit_spin_lock",
        "__bit_spin_lock",
    };
    for (const char *e : exact)
        if (name == e)
            return true;
    return false;
}

// Returns true if a callee name is a CPU-local serialization call (H4).
static bool isIrqDisableCall(const StringRef name)
{
    static const char *const names[] = {
        "local_irq_disable",
        "local_bh_disable",
        "_local_bh_disable",
        "__local_bh_disable",
        "local_irq_save",
        // aarch64: local_bh_disable() is fully inlined (preempt_count_add);
        // the re-enable path is NOT inlined and appears as a real call.
        // Presence of local_bh_enable/_enable_ip is a sound proxy: any fn
        // that re-enables BH must have disabled it first.
        "local_bh_enable",
        "__local_bh_enable_ip",
        // aarch64 inline expansion: local_bh_disable() → __local_bh_disable_ip()
        "__local_bh_disable_ip",
        // x86 inline expansion: local_irq_save() → __raw_local_irq_save()
        "__raw_local_irq_save",
    };
    for (const char *e : names)
        if (name == e)
            return true;
    return false;
}

// Returns true if a callee name is a per-line IRQ sync call (H20).
static bool isIrqSyncCall(const StringRef name)
{
    return name == "disable_irq" || name == "synchronize_irq" ||
           name == "disable_irq_nosync";
}

// Returns true if a callee name is an RCU / SRCU reader entry (H3 + H16).
static bool isRcuReadLock(const StringRef name)
{
    return name == "rcu_read_lock"     || name == "__rcu_read_lock" ||
           name == "srcu_read_lock"    || name == "srcu_read_lock_held" ||
           // aarch64 inline expansion: srcu_read_lock() → __srcu_read_lock()
           name == "__srcu_read_lock" ||
           name == "rcu_read_lock_bh" || name == "rcu_read_lock_sched";
}

// The seqlock API is all static inline, so there is no callee to match on.
// What survives into the IR is the embedded spinlock on the write side and a
// plain load of the counter on the read side.
static bool isSeqlockObject(const GlobalVariable *G)
{
    Type *T = G->getValueType();
    if (auto *ST = dyn_cast<StructType>(T))
        if (ST->hasName()) {
            StringRef n = ST->getName();
            return n.contains("seqlock_t") || n.contains("seqcount");
        }
    return false;
}

static const GlobalVariable *baseGlobalOf(const Value *V)
{
    const Value *cur = V->stripPointerCasts();
    while (auto *GEP = dyn_cast<GEPOperator>(cur))
        cur = GEP->getPointerOperand()->stripPointerCasts();
    return dyn_cast<GlobalVariable>(cur);
}

static bool isSpinLockCall(const StringRef name)
{
    return name.starts_with("_raw_spin_lock") ||
           name.starts_with("_raw_spin_unlock") ||
           name.starts_with("_raw_write_lock") ||
           name.starts_with("_raw_write_unlock");
}

// Returns true if a callee name is an RCU / SRCU grace-period call (H3+H16).
//
// NOTE: call_rcu / call_srcu are intentionally excluded.  Those functions
// schedule a callback asynchronously — they do NOT block and return before
// the grace period elapses.  The calling function therefore CAN run
// concurrently with active RCU readers.  Only the synchronize_* variants
// block until all prior readers have exited.
static bool isRcuSyncCall(const StringRef name)
{
    return name == "synchronize_rcu"           ||
           name == "synchronize_rcu_expedited" ||
           name == "synchronize_srcu"          ||
           name == "synchronize_srcu_expedited";
}

// Returns true if a callee is a kernel completion signal (H14).
static bool isCompletionSignal(const StringRef name)
{
    return name == "complete"          ||
           name == "complete_all"      ||
           name == "complete_and_exit";
}


static bool isCompletionWait(const StringRef name)
{
    return name == "wait_for_completion"               ||
           name == "wait_for_completion_interruptible" ||
           name == "wait_for_completion_killable"      ||
           name == "wait_for_completion_io"            ||
           name.starts_with("wait_for_completion_timeout");
}

// ---------------------------------------------------------------------------
// IrqSerialHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string IrqSerialHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    // H4 ("both fns call local_bh_disable") is intentionally omitted.
    // local_bh_disable / local_irq_disable are CPU-local: they prevent
    // reentrancy on the SAME CPU only.  Two functions on different CPUs with
    // BH disabled are still fully concurrent.  NB patterns (smp_store_release
    // / smp_load_acquire) exist precisely for cross-CPU visibility — so H4
    // can never be a sound filter for any pair this tool reports.

    // H20: one explicitly serializes an IRQ line; the other is an IRQ handler
    // (name contains _irq_, _isr_, or _interrupt_).
    auto isIrqHandler = [](const std::string &n) -> bool {
        auto endswith = [&](const char *s) {
            size_t sl = strlen(s);
            return n.size() >= sl && n.compare(n.size() - sl, sl, s) == 0;
        };
        return n.find("_irq_")       != std::string::npos ||
               n.find("_isr_")       != std::string::npos ||
               n.find("_interrupt_") != std::string::npos ||
               endswith("_irq")   ||
               endswith("_isr")   ||
               endswith("_interrupt");
    };
    bool aSync    = irqSyncCallers_.count(pair.anchorFn)  > 0;
    bool bSync    = irqSyncCallers_.count(pair.partnerFn) > 0;
    bool aHandler = isIrqHandler(pair.anchorFn);
    bool bHandler = isIrqHandler(pair.partnerFn);
    if (aSync && bHandler) return "irq-sync+handler";
    if (bSync && aHandler) return "irq-sync+handler";
    return "";
}

// ---------------------------------------------------------------------------
// SubsystemLockHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string SubsystemLockHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    auto itA = subsysLocks_.find(pair.anchorFn);
    auto itB = subsysLocks_.find(pair.partnerFn);
    if (itA == subsysLocks_.end() || itB == subsysLocks_.end())
        return "";
    // Mutually exclusive if both call the same subsystem lock function.
    for (const auto &lockName : itA->second)
        if (itB->second.count(lockName))
            return lockName;
    return "";
}

// ---------------------------------------------------------------------------
// PerCpuHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string PerCpuHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    auto itA = perCpuSets_.find(pair.anchorFn);
    auto itB = perCpuSets_.find(pair.partnerFn);
    if (itA == perCpuSets_.end() || itB == perCpuSets_.end())
        return "";
    // Both access the same per-CPU variable — not a shared-memory race.
    for (unsigned id : itA->second)
        if (itB->second.count(id))
            return "per-cpu";
    return "";
}

// ---------------------------------------------------------------------------
// SafeAllocationHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string SafeAllocationHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    // H24: both use regmap APIs (all regmap calls are internally locked).
    if (regmapFns_.count(pair.anchorFn) && regmapFns_.count(pair.partnerFn))
        return "regmap";

    // H21: both use DMA-sync barriers protecting the shared buffer.
    if (dmaSyncFns_.count(pair.anchorFn) && dmaSyncFns_.count(pair.partnerFn))
        return "dma-sync";

    // H15: seqlock reader/writer pair — benign by design.
    bool aRd = seqlockRdFns_.count(pair.anchorFn)  > 0;
    bool aWr = seqlockWrFns_.count(pair.anchorFn)  > 0;
    bool bRd = seqlockRdFns_.count(pair.partnerFn) > 0;
    bool bWr = seqlockWrFns_.count(pair.partnerFn) > 0;
    if ((aRd && bWr) || (aWr && bRd))
        return "seqlock-rw";

    // H11 + H25: check object descriptions for known-benign names / IOMEM.
    auto isBenignGlobal = [](const std::string &desc) -> bool {
        static const char *const benignGlobals[] = {
            "jiffies", "runqueues", "cpu_info", "kstat",
        };
        for (const char *b : benignGlobals)
            if (desc.find(b) != std::string::npos)
                return true;
        return false;
    };
    auto isIomem = [](const std::string &desc) -> bool {
        static const char *const iomemSuffixes[] = {
            "_regs", "_base", "_iobase", "_mmio", "_bar", "_csr",
        };
        for (const char *s : iomemSuffixes) {
            size_t slen = strlen(s);
            if (desc.size() >= slen &&
                desc.compare(desc.size() - slen, slen, s) == 0)
                return true;
        }
        return false;
    };
    const AnchorBracket &br = pair.bracket;
    auto checkDesc = [&](const std::string &desc) -> std::string {
        if (isBenignGlobal(desc)) return "benign-global";
        if (isIomem(desc))        return "iomem";
        return "";
    };
    if (br.anchorAlloc) { auto s = checkDesc(br.anchorAlloc->ptrDesc); if (!s.empty()) return s; }
    if (br.before)      { auto s = checkDesc(br.before->objDesc);      if (!s.empty()) return s; }
    if (br.after)       { auto s = checkDesc(br.after->objDesc);       if (!s.empty()) return s; }
    for (const auto &pa : pair.partnerAccesses) {
        auto s = checkDesc(pa.objDesc);
        if (!s.empty()) return s;
    }

    return "";
}

// ---------------------------------------------------------------------------
// ConservativeAnyLockHeuristic::isMutuallyExclusive
// ---------------------------------------------------------------------------
std::string ConservativeAnyLockHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    // H2p: suppress only if BOTH functions acquire some exclusive lock.
    if (anyLockFns_.count(pair.anchorFn) && anyLockFns_.count(pair.partnerFn))
        return "any-lock";
    return "";
}


// ---------------------------------------------------------------------------
// BenignAnchorHeuristic::isMutuallyExclusive  (H_BA)
//
// Suppresses pairs whose ordering anchor is a known-benign call: the function
// performs an atomic internally (so kExtFnSummary tags it as ordered), but the
// ordering is private to its own implementation, not a synchronisation primitive
// for the CALLER's shared data.  These anchors generate structural false
// positives because the analyser cannot prove the absence of a happens-before
// relationship from a generic utility call.
//
// Detection works in two layers (label format = toLower(name) + " (ext)"):
//   1. Exact label match for well-known unique helpers.
//   2. Prefix/suffix match for whole families of allocators / deallocators /
//      string utilities / printk / random / time helpers.
//
// Empirical effect on the 48-case kernel-noLTO corpus:
//   Cases 6, 14–18, 34, 35 (8 FPs) flip from No → suppressed.
//
// SAFETY: anchors that ARE genuine sync primitives MUST NOT appear here.
//   - smp_store_release / smp_load_acquire / smp_*mb / smp_rmb / smp_wmb
//   - cmpxchg / xchg / atomic_*_release / atomic_*_acquire
//   - rcu_assign_pointer / rcu_dereference (RCU publish)
//   - synchronize_rcu / synchronize_srcu (grace period)
//   - wait_for_completion / complete (handled by CompletionOrderHeuristic)
//   - spin_lock/unlock, mutex_lock/unlock, down/up (handled by LockAlias)
//   - schedule_work / queue_work — these ARE ordering primitives that publish
//                                   data to a worker, so NOT benign here.
//   - call_rcu — INCLUDED (callback defer; for the CALLER's currently-published
//                data it provides no useful ordering with concurrent readers;
//                callees inside the callback are a separate analysis problem).
// ---------------------------------------------------------------------------
namespace {

// Extract bare function name from an anchor label of the form
// "<lowercase-fnname> (ext)".  Returns empty string for non-ext labels.
static std::string extLabelToName(const std::string &label) {
    constexpr llvm::StringLiteral kSfx = " (ext)";
    if (label.size() <= kSfx.size()) return {};
    if (label.compare(label.size() - kSfx.size(), kSfx.size(),
                      kSfx.data(), kSfx.size()) != 0)
        return {};
    return label.substr(0, label.size() - kSfx.size());
}

// Match a function name against a family of benign helpers via prefix/exact.
// Returns the category string (used as sub-category) or empty.
static std::string classifyBenignFnName(const std::string &name) {
    auto starts = [&](const char *p) {
        size_t l = std::strlen(p);
        return name.size() >= l && name.compare(0, l, p) == 0;
    };
    auto eq = [&](const char *p) { return name == p; };

    // --- printk / warn / BUG / debug log ---
    if (eq("_printk") || eq("__warn_printk") || eq("printk_deferred") ||
        starts("pr_warn_once") || starts("pr_err_once") ||
        starts("pr_info_once") || eq("dump_stack") ||
        eq("__might_sleep") || eq("___might_sleep") ||
        eq("might_resched"))
        return "log/warn";

    // --- random / entropy ---
    if (eq("get_random_u64") || eq("get_random_u32") ||
        eq("get_random_u16") || eq("get_random_u8") ||
        eq("get_random_bytes") || eq("get_random_bytes_arch") ||
        eq("prandom_u32"))
        return "rng";

    // --- time / jiffies / ktime ---
    if (starts("jiffies_to_") || starts("usecs_to_jiffies") ||
        starts("msecs_to_jiffies") || starts("nsecs_to_jiffies") ||
        starts("ktime_get") || eq("ktime_get_real_ts64") ||
        eq("ktime_get_coarse") || eq("ktime_to_us") || eq("ktime_to_ms") ||
        eq("ktime_to_ns") || eq("get_jiffies_64"))
        return "time";

    // --- allocators (size-only side effects; no caller-visible publish) ---
    if (starts("kmalloc") || starts("__kmalloc") ||
        starts("kzalloc") || starts("kcalloc") ||
        starts("kvmalloc") || starts("__kvmalloc") || starts("kvzalloc") ||
        starts("vmalloc") || starts("vzalloc") ||
        starts("devm_kmalloc") || starts("devm_kzalloc") ||
        starts("devm_kvmalloc") || starts("devm_kcalloc") ||
        starts("kmem_cache_alloc") ||
        eq("__get_free_pages") || eq("get_zeroed_page") ||
        starts("__alloc_pages") || starts("__alloc_skb") ||
        eq("alloc_pages_exact") || eq("__page_frag_cache_drain") ||
        starts("bitmap_alloc") || starts("bitmap_zalloc"))
        return "alloc";

    // --- deallocators ---
    if (eq("kfree") || eq("kvfree") || eq("vfree") ||
        eq("kmem_cache_free") || eq("kmem_cache_free_bulk") ||
        starts("__free_pages") || eq("free_pages") || eq("free_pages_exact") ||
        eq("devm_kfree") || eq("devm_kvfree") ||
        starts("bitmap_free") || eq("kfree_skb") || eq("consume_skb") ||
        eq("kfree_skb_reason") || eq("dev_kfree_skb") ||
        eq("dev_kfree_skb_any") || eq("dev_kfree_skb_irq"))
        return "dealloc";

    // --- string / mem utilities (compiler builtins; not sync) ---
    if (starts("memcpy") || starts("memset") || starts("memmove") ||
        starts("memcmp") || starts("memchr") ||
        starts("__memcpy") || starts("__memset") || starts("__memmove") ||
        starts("strcpy") || starts("strncpy") || starts("strscpy") ||
        starts("strlcpy") || starts("strlcat") ||
        eq("strlen") || eq("strnlen") || eq("strcmp") || eq("strncmp") ||
        eq("strchr") || eq("strrchr") || eq("strstr") || eq("strcasecmp") ||
        starts("sized_strscpy") ||
        starts("snprintf") || starts("scnprintf") || starts("sprintf") ||
        starts("vsnprintf") || starts("vscnprintf") || starts("vsprintf"))
        return "string/mem";

    // --- kernel object naming / refcount-init only (no race-relevant write) ---
    if (eq("dev_set_name") || eq("kobject_set_name") ||
        eq("device_set_name") || eq("kref_init") ||
        eq("refcount_set"))
        return "obj-name";

    // --- callback-defer (call_rcu) — caller's data not synchronised ---
    if (eq("call_rcu") || eq("call_srcu") || eq("call_rcu_tasks") ||
        eq("queue_rcu_work"))
        return "callback-defer";

    return {};
}

} // anonymous namespace

std::string SeqlockRetryHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    bool aW = writers_.count(pair.anchorFn)  > 0;
    bool aR = readers_.count(pair.anchorFn)  > 0;
    bool bW = writers_.count(pair.partnerFn) > 0;
    bool bR = readers_.count(pair.partnerFn) > 0;

    if ((aW && bR) || (aR && bW)) return "seqlock-write+retry";
    if (aW && bW)                 return "seqlock-write+write";
    return "";
}

std::string BenignAnchorHeuristic::isMutuallyExclusive(
    const FunctionPair      &pair,
    Module                  & /*M*/,
    ModuleAnalysisManager   & /*MAM*/) const
{
    const std::string &label = pair.bracket.anchor.label;

    // 1. Exact-label fast path (legacy, unique helpers)
    static const char *const kBenignLabels[] = {
        "__warn_printk (ext)",
        "_printk (ext)",
        "kfree (ext)",
        "get_random_u64 (ext)",
        "bitmap_zalloc (ext)",
        "dev_set_name (ext)",
        nullptr
    };
    for (const char *const *p = kBenignLabels; *p; ++p)
        if (label == *p)
            return *p;

    // 2. Family classification for "(ext)" anchors.
    std::string name = extLabelToName(label);
    if (!name.empty()) {
        std::string cat = classifyBenignFnName(name);
        if (!cat.empty())
            return cat;
    }
    return "";
}


FnHeuristicsFilterResult
FnHeuristicsFilterPass::run(Module &M, ModuleAnalysisManager &MAM)
{
    mbtime::Scope _t("mutex_filtering");

    FnHeuristicsFilterResult result;

    // ── 1. Obtain FunctionPairs ────────────────────────────────────────────
    const FunctionPairsResult &pairs = MAM.getResult<FunctionPairsPass>(M);
    if (pairs.byAnchorFn.empty())
        return result;

    // ── 2. Non-SVF IR scan ─────────────────────────────────────────────────
    // Single pass over all defined functions to collect data for all
    // name/section/call-pattern heuristics that do not need PTA.

    std::set<std::string> initOnly, exitOnly;     // H5, H6, H12, H17, H19, H23
    std::set<std::string> rcuReaders, rcuSync;    // H3 + H16
    std::set<std::string> seqWriters, seqReaders;
    std::set<std::string> irqDisablers;           // H4
    std::set<std::string> irqSyncCallers;         // H20
    std::map<std::string, std::set<std::string>> subsysLocks; // H13
    std::set<std::string> anyLockFns;             // H2p
    std::set<std::string> regmapFns;              // H24
    std::set<std::string> dmaSyncFns;             // H21
    std::set<std::string> seqlockRdFns;           // H15
    std::set<std::string> seqlockWrFns;           // H15
    // G5.2: handler functions registered as IRQ / tasklet / work / timer /
    // kthread callbacks.  Populated below from call-site mining of the
    // registration APIs; propagated transitively in a fixpoint loop so that
    // any function reachable from a handler is also tagged.  Consumed by
    // CallGraphHeuristic to avoid false suppression when call-graph
    // reachability spans an asynchronous context boundary.
    std::set<std::string> irqReachable;
    // G5.4: sysfs / fops runtime callbacks — functions that the kernel
    // invokes from userspace I/O paths after probe/bind completes.  These
    // are categorically NOT init-only or exit-only; they run at runtime
    // concurrently with everything else.  Populated by name-suffix matching
    // and used to *remove* spurious entries from moduleInitOnly / moduleExitOnly
    // after H27 propagation, in case a sysfs callback happens to be reachable
    // only from a probe path in this particular translation unit.
    std::set<std::string> sysfsRuntime;

    // Zero-argument global subsystem lock names (H13).
    static const char *const kSubsysLocks[] = {
        "rtnl_lock",
        "netdev_lock",
        "i2c_lock_bus",
        "spi_bus_lock",
        "pci_lock_rescan_remove",
        "device_lock",
        "acpi_bus_lock",
        "usb_lock_device",
        // VT/console driver operations are serialised by the console semaphore;
        // console_lock() is a zero-arg global wrapper for down(&console_sem).
        "console_lock",
        "console_trylock",
    };

    // Name patterns that imply init-only lifecycle (H5 + H12).
    //
    // FN-AUDIT-v9 narrowing: only true module-init markers remain.  Suffixes
    // that previously appeared here (_probe, _attach, _bind, _resume,
    // _resume_noirq/_early, _restore*, _thaw*, _powerup, _runtime_resume)
    // have been removed.  Reasoning:
    //   - _probe / _attach / _bind are RECURRING per-device callbacks invoked
    //     every time a device binds.  Two different devices in the same TU
    //     can be probed concurrently on different CPUs.  Cross-pair
    //     suppression via "init+exit" was an empirical FN source for hot-plug
    //     bus drivers.
    //   - _resume / _suspend (and all PM phase variants) are RECURRING per
    //     suspend/resume CYCLE.  A driver writer can publish state in
    //     _resume that an IRQ handler running between two cycles will read.
    //     Classifying them as one-shot init/exit suppressed legitimate
    //     across-cycle races.
    // These suffixes are instead added to sysfsRuntime below so H27 transitive
    // propagation cannot inherit "module-init-only" through them either.
    auto isInitName = [](StringRef n) -> bool {
        return n.starts_with("__initstub__");   // module_init macro expansion
    };

    // Name patterns that imply exit-only lifecycle.
    //
    // FN-AUDIT-v9 narrowing: only terminal-release callbacks (kref/kobject
    // final destruction; module .exit-section cleanup) remain.  Removed:
    //   - _remove / _detach / _unbind / _suspend* / _freeze* / _powerdown /
    //     _runtime_suspend: see isInitName comment — recurring runtime
    //     callbacks, not one-shot module-exit.
    // _kref_release / _kobject_release / _free_dev / _cleanup are kept
    // because they are FINAL teardown — refcount reached zero, no further
    // access can be made to the object.
    auto isExitName = [](StringRef n) -> bool {
        return n.ends_with("_kref_release")     ||  // kref final release callback
               n.ends_with("_kobject_release")  ||  // kobject final release
               n.ends_with("_free_dev")         ||  // device free callback
               n.ends_with("_cleanup");             // explicit teardown
    };

    // FN-AUDIT-v9: recurring driver-model and PM callbacks.  These are NOT
    // init/exit but the kernel invokes them through the driver core at
    // arbitrary points in module life; we must keep them out of
    // moduleInitOnly/moduleExitOnly so H27 propagation cannot mark a
    // function reachable from one of them as init/exit-only.
    auto isRuntimeRecurringName = [](StringRef n) -> bool {
        return n.ends_with("_probe")           ||
               n.ends_with("_attach")          ||
               n.ends_with("_bind")            ||
               n.ends_with("_remove")          ||
               n.ends_with("_detach")          ||
               n.ends_with("_unbind")          ||
               // PM system-wide
               n.ends_with("_resume")          ||
               n.ends_with("_resume_noirq")    ||
               n.ends_with("_resume_early")    ||
               n.ends_with("_restore")         ||
               n.ends_with("_restore_noirq")   ||
               n.ends_with("_restore_early")   ||
               n.ends_with("_thaw")            ||
               n.ends_with("_thaw_noirq")      ||
               n.ends_with("_thaw_early")      ||
               n.ends_with("_suspend")         ||
               n.ends_with("_suspend_noirq")   ||
               n.ends_with("_suspend_late")    ||
               n.ends_with("_suspend_early")   ||
               n.ends_with("_freeze")          ||
               n.ends_with("_freeze_noirq")    ||
               n.ends_with("_freeze_late")     ||
               // PM runtime + power-rail
               n.ends_with("_powerup")         ||
               n.ends_with("_powerdown")       ||
               n.ends_with("_runtime_resume")  ||
               n.ends_with("_runtime_suspend") ||
               n.ends_with("_runtime_idle");
    };

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        const std::string fname = F.getName().str();

        // ── Seqlock: writer holds the embedded spinlock, reader only
        //    loads the counter and retries ─────────────────────────────────
        {
            std::set<const GlobalVariable *> locked, loaded;
            for (BasicBlock &BB : F)
                for (Instruction &I : BB) {
                    if (auto *CI = dyn_cast<CallInst>(&I)) {
                        const Function *cf = CI->getCalledFunction();
                        if (cf && isSpinLockCall(cf->getName()) &&
                            CI->arg_size() > 0)
                            if (const GlobalVariable *G =
                                    baseGlobalOf(CI->getArgOperand(0)))
                                if (isSeqlockObject(G))
                                    locked.insert(G);
                    } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                        if (const GlobalVariable *G =
                                baseGlobalOf(LI->getPointerOperand()))
                            if (isSeqlockObject(G))
                                loaded.insert(G);
                    }
                }
            if (!locked.empty())
                seqWriters.insert(fname);
            for (const GlobalVariable *G : loaded)
                if (!locked.count(G)) { seqReaders.insert(fname); break; }
        }

        // ── Lifecycle: section-based (.init.text / .exit.text) ───────────
        StringRef sec = F.getSection();
        bool inInitSec = sec.contains(".init.text");
        bool inExitSec = sec.contains(".exit.text");
        if (inInitSec || isInitName(F.getName()))
            initOnly.insert(fname);
        if (inExitSec || isExitName(F.getName()))
            exitOnly.insert(fname);

        // ── G5.4 + FN-AUDIT-v9: sysfs / fops / fs / vm / driver-model
        //    runtime callbacks ─────────────────────────────────────────────
        // The kernel invokes these from userspace I/O paths, the page-cache,
        // VFS, MM, IRQ subsystem, or driver-model dispatch — NOT during a
        // single init or exit phase.  Tag them so H27 propagation cannot
        // mark them (or anything reachable only from them) module-init-only
        // or module-exit-only.
        {
            StringRef n = F.getName();
            if (// sysfs / fops (G5.4)
                n.ends_with("_show")        || n.ends_with("_store")     ||
                n.ends_with("_open")        || n.ends_with("_release")   ||
                n.ends_with("_ioctl")       || n.ends_with("_unlocked_ioctl") ||
                n.ends_with("_compat_ioctl")|| n.ends_with("_mmap")      ||
                n.ends_with("_poll")        || n.ends_with("_fasync")    ||
                n.ends_with("_llseek")      || n.ends_with("_fsync")     ||
                n.ends_with("_proc_show")   || n.ends_with("_proc_open") ||
                // FN-AUDIT-v9: file_operations read/write paths
                n.ends_with("_read_iter")   || n.ends_with("_write_iter")||
                n.ends_with("_iter_read")   || n.ends_with("_iter_write")||
                n.ends_with("_splice_read") || n.ends_with("_splice_write") ||
                // FN-AUDIT-v9: address_space_operations
                n.ends_with("_readpage")    || n.ends_with("_writepage") ||
                n.ends_with("_readahead")   || n.ends_with("_writepages")||
                n.ends_with("_releasepage") || n.ends_with("_invalidatepage") ||
                n.ends_with("_set_page_dirty") ||
                n.ends_with("_dirty_folio") || n.ends_with("_release_folio") ||
                n.ends_with("_invalidate_folio") ||
                // FN-AUDIT-v9: super_operations
                n.ends_with("_destroy_inode") ||
                n.ends_with("_evict_inode")   ||
                n.ends_with("_drop_inode")    ||
                n.ends_with("_dirty_inode")   ||
                n.ends_with("_write_inode")   ||
                n.ends_with("_put_super")     ||
                n.ends_with("_sync_fs")       ||
                n.ends_with("_statfs")        ||
                // FN-AUDIT-v9: vm_operations_struct
                n.ends_with("_fault")         ||
                n.ends_with("_nopage")        ||
                n.ends_with("_page_mkwrite")  ||
                n.ends_with("_huge_fault")    ||
                // FN-AUDIT-v9: driver-model + PM (recurring callbacks)
                isRuntimeRecurringName(n)     ||
                // IRQ / tasklet / softirq handlers (naming convention)
                n.ends_with("_isr")           ||
                n.ends_with("_interrupt")     ||
                n.ends_with("_handler")       ||
                n.ends_with("_irq_handler")   ||
                n.ends_with("_threaded_irq")  ||
                // Workqueue / timer / kthread callbacks
                n.ends_with("_work_fn")       ||
                n.ends_with("_workfn")        ||
                n.ends_with("_timer_fn")      ||
                n.ends_with("_kthread"))
                sysfsRuntime.insert(fname);
        }

        // ── Call instruction scan ─────────────────────────────────────────
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                const auto *CI = dyn_cast<CallInst>(&I);
                if (!CI)
                    continue;
                const Function *callee = CI->getCalledFunction();
                if (!callee)
                    continue;
                StringRef cn = callee->getName();

                // H3 + H16: RCU / SRCU
                if (isRcuReadLock(cn))   rcuReaders.insert(fname);
                if (isRcuSyncCall(cn))   rcuSync.insert(fname);

                // H4: local IRQ / BH disable
                if (isIrqDisableCall(cn)) irqDisablers.insert(fname);

                // H20: per-line IRQ sync
                if (isIrqSyncCall(cn)) irqSyncCallers.insert(fname);

                // H13: zero-arg subsystem locks
                for (const char *sl : kSubsysLocks)
                    if (cn == sl)
                        subsysLocks[fname].insert(cn.str());

                // H2p: any exclusive lock (conservative catch-all)
                if (isExclusiveLockAcquire(cn))
                    anyLockFns.insert(fname);

                // H24: regmap APIs
                if (cn.starts_with("regmap_read") ||
                    cn.starts_with("regmap_write") ||
                    cn.starts_with("regmap_update"))
                    regmapFns.insert(fname);

                // H21: DMA sync barriers
                if (cn.starts_with("dma_sync_") ||
                    // aarch64: dma_sync_single_for_cpu/device inlines to __dma_sync_single_for_*
                    cn.starts_with("__dma_sync_"))
                    dmaSyncFns.insert(fname);

                // H15: seqlock reader / writer
                if (cn == "read_seqbegin" || cn == "read_seqretry" ||
                    cn == "__read_seqbegin" || cn == "__read_seqretry")
                    seqlockRdFns.insert(fname);
                if (cn == "write_seqlock"    || cn == "write_seqlock_irq" ||
                    cn == "write_seqlock_bh" || cn == "write_seqlock_irqsave")
                    seqlockWrFns.insert(fname);

                // G5.2: mine async-context registration sites for handler
                // function pointers and seed the IRQ-reachable set.  The
                // handler-function argument index is API-specific.
                //
                //   request_irq(irq, handler,         flags, name, dev)         → arg 1
                //   request_threaded_irq(irq, h, th,  flags, name, dev)         → arg 1 and 2
                //   devm_request_irq(dev, irq, handler, flags, name, p)         → arg 2
                //   devm_request_threaded_irq(d, i, h, th, flags, name, p)      → arg 2 and 3
                //   tasklet_init(t, func, data)                                 → arg 1
                //   tasklet_setup(t, callback)                                  → arg 1
                //   INIT_WORK / INIT_DELAYED_WORK → __INIT_WORK_WITH_KEY(w, fn) → arg 1
                //   kthread_create / kthread_run(fn, ...)                       → arg 0
                //   hrtimer_init / timer_setup is handled via timer_init_key
                //
                // We use callee-name + arg index; the function-pointer arg
                // is matched via dyn_cast<Function>(operand stripped of casts).
                auto seedIrqHandler = [&](unsigned argIdx) {
                    if (argIdx >= CI->arg_size()) return;
                    const Value *v = CI->getArgOperand(argIdx)->stripPointerCasts();
                    if (const auto *fn = dyn_cast<Function>(v))
                        irqReachable.insert(fn->getName().str());
                };
                if (cn == "request_irq")              seedIrqHandler(1);
                else if (cn == "request_threaded_irq") { seedIrqHandler(1); seedIrqHandler(2); }
                else if (cn == "devm_request_irq")    seedIrqHandler(2);
                else if (cn == "devm_request_threaded_irq") { seedIrqHandler(2); seedIrqHandler(3); }
                else if (cn == "tasklet_init")        seedIrqHandler(1);
                else if (cn == "tasklet_setup")       seedIrqHandler(1);
                else if (cn == "__INIT_WORK_WITH_KEY" ||
                         cn == "INIT_WORK_ONSTACK" ||
                         cn == "init_work" ||
                         cn == "__init_work")         seedIrqHandler(1);
                else if (cn == "kthread_create" ||
                         cn == "kthread_create_on_node" ||
                         cn == "kthread_run")         seedIrqHandler(0);
                else if (cn == "init_irq_work" ||
                         cn == "__init_irq_work")     seedIrqHandler(1);
                // G5.5: timer / hrtimer / delayed work runtime callback APIs.
                // Their callback is invoked asynchronously from a softirq
                // context, so the function runs at runtime regardless of
                // when it was scheduled.
                else if (cn == "timer_setup" ||
                         cn == "timer_setup_on_stack" ||
                         cn == "__init_timer" ||
                         cn == "__init_timer_on_stack" ||
                         cn == "hrtimer_setup" ||
                         cn == "hrtimer_setup_on_stack" ||
                         cn == "__hrtimer_init" ||
                         cn == "__hrtimer_init_sleeper" ||
                         cn == "__init_delayed_work" ||
                         cn == "INIT_DELAYED_WORK_ONSTACK_KEY" ||
                         cn == "__INIT_DELAYED_WORK_WITH_KEY")
                                                      seedIrqHandler(1);

                // H_BA+H27: object-initialisation indicator functions.
                // If a function calls one of these helpers, it is itself in
                // the init lifecycle phase.  This catches driver protocol
                // callbacks stored as function pointers (H27 propagation
                // cannot reach them via the static call graph).
                static const char *const kInitIndicatorFns[] = {
                    "timer_init_key",        // struct timer_list init
                    "tty_port_init",         // struct tty_port init
                    "mutex_init_generic",    // mutex_init macro expansion
                    "__init_waitqueue_head", // wait_queue_head_t init
                    "device_register",       // device probe / bus bind
                    nullptr
                };
                for (const char *const *q = kInitIndicatorFns; *q; ++q)
                    if (cn == *q)
                        initOnly.insert(fname);
            }
        }
    }

    // ── 2b. Transitive module-init-only / module-exit-only propagation (H27) ──
    //
    // Build call-graph edges for internal-linkage functions only.  We then
    // propagate initOnly / exitOnly membership transitively: if a function is
    // only ever called (directly) by init-only functions (and is not
    // externally callable), it is itself module-init-only.  Symmetric for
    // exit-only.  Functions with external / hidden linkage can be called from
    // outside the module and are therefore never classified here.
    //
    // We use a simple fixpoint over a reverse call-graph (callee → callers).
    //
    // Algorithm:
    //   1. Collect all direct callers of each internal function.
    //   2. A function f is moduleInitOnly if:
    //        - f is internal linkage, AND
    //        - every direct caller of f is in initOnly ∪ moduleInitOnly, AND
    //        - f has no external-linkage callers in the module (or zero callers
    //          from outside the init closure).
    //   3. Repeat until no new functions are added.
    //   Symmetric for moduleExitOnly.

    // Map: callee name → set of caller names (for functions in this module).
    std::map<std::string, std::set<std::string>> callers;
    // Also track which functions are internal (non-exported) linkage.
    std::set<std::string> internalFns;

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        bool isInternal = F.hasInternalLinkage() || F.hasPrivateLinkage();
        if (isInternal)
            internalFns.insert(F.getName().str());
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                const auto *CI = dyn_cast<CallInst>(&I);
                if (!CI) continue;
                const Function *callee = CI->getCalledFunction();
                if (!callee || callee->isDeclaration()) continue;
                // Only trace internal callees (external fns are not candidates).
                if (callee->hasInternalLinkage() || callee->hasPrivateLinkage())
                    callers[callee->getName().str()].insert(F.getName().str());
            }
        }
    }

    std::set<std::string> moduleInitOnly;
    std::set<std::string> moduleExitOnly;

    // Initialise with the existing init/exit sets.
    moduleInitOnly = initOnly;
    moduleExitOnly = exitOnly;

    // Fixpoint propagation: repeatedly expand moduleInitOnly / moduleExitOnly.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[callee, callerSet] : callers) {
            // Skip if not internal.
            if (!internalFns.count(callee)) continue;
            // Already classified — skip.
            bool alreadyInit = moduleInitOnly.count(callee) > 0;
            bool alreadyExit = moduleExitOnly.count(callee) > 0;

            if (!alreadyInit) {
                // Candidate for moduleInitOnly: every caller must be in
                // moduleInitOnly AND the callee must have at least one caller
                // (orphan internal functions are ignored).
                bool allCallersInit = !callerSet.empty();
                for (const auto &cr : callerSet) {
                    if (!moduleInitOnly.count(cr)) {
                        allCallersInit = false;
                        break;
                    }
                }
                if (allCallersInit) {
                    moduleInitOnly.insert(callee);
                    changed = true;
                }
            }
            if (!alreadyExit) {
                bool allCallersExit = !callerSet.empty();
                for (const auto &cr : callerSet) {
                    if (!moduleExitOnly.count(cr)) {
                        allCallersExit = false;
                        break;
                    }
                }
                if (allCallersExit) {
                    moduleExitOnly.insert(callee);
                    changed = true;
                }
            }
        }
    }

    // Remove functions that are in BOTH sets — they are transitively reachable
    // from both init and exit, meaning they run at multiple lifecycle phases.
    // Classifying them as exclusively one phase would be unsound.
    for (const auto &fn : moduleExitOnly)
        moduleInitOnly.erase(fn);
    // (moduleExitOnly entries that are also init are ambiguous; keep exitOnly
    // since exit-phase suppression is conservative — suppress both directions.)

    // G5.4 defensive guard: sysfs / fops runtime callbacks must NEVER be
    // marked module-init-only or module-exit-only.  Even if the static call
    // graph in this TU reaches them only from a probe path (because their
    // call sites are indirect through a vtable like file_operations or
    // device_attribute), they run at userspace-I/O time concurrently with
    // any other runtime code.  Strip them post-fixpoint.
    for (const auto &fn : sysfsRuntime) {
        moduleInitOnly.erase(fn);
        moduleExitOnly.erase(fn);
        // Also strip from the seed init/exit sets if they crept in via
        // suffix collision (e.g. a function named "foo_open" that is
        // genuinely init-only would lose its tag — accepted as the
        // conservative trade-off; misclassifying a sysfs handler is far
        // worse than missing a static init suppression).
        initOnly.erase(fn);
        exitOnly.erase(fn);
    }

    // G5.5: static-initializer async-callback seeding.
    //
    // Many kernel runtime callbacks are registered via static initializer
    // macros that do NOT generate runtime API calls: DECLARE_WORK,
    // DECLARE_DELAYED_WORK, DEFINE_TIMER, etc. all expand to a global
    // variable of a known struct type whose .func / .function field
    // statically holds the callback's address.  The earlier call-site
    // seeding misses these.  Walk the module's globals using TYPED descent:
    // we only collect Function* constants that appear WITHIN a known
    // async-callback struct subtree, never from sibling fields of an
    // outer aggregate.  Sibling-walking would over-classify ordinary
    // driver vtables (file_operations, psmouse_protocol, etc.) that
    // happen to coexist with an async struct in the same global.
    //
    // Soundness: every Function* we collect is reachable through an
    // async-callback field of an async-callback struct → invoked by the
    // worker/timer/IRQ subsystem at runtime.
    {
        auto isAsyncStructName = [](StringRef nm) -> bool {
            static const char *const kAsyncStructs[] = {
                "struct.work_struct",
                "struct.delayed_work",
                "struct.rcu_work",
                "struct.timer_list",
                "struct.hrtimer",
                "struct.hrtimer_sleeper",
                "struct.tasklet_struct",
                "struct.irq_work",
                nullptr
            };
            for (const char *const *p = kAsyncStructs; *p; ++p) {
                StringRef base(*p);
                if (nm == base) return true;
                // LLVM appends ".N" for type duplicates: "struct.work_struct.42"
                if (nm.starts_with(base) && nm.size() > base.size() &&
                    nm[base.size()] == '.')
                    return true;
            }
            return false;
        };
        // Walk-any: collect every Function* in any nested constant operand.
        // A visited set prevents infinite recursion on self-referential
        // global initializers (e.g. perf's @perf_event_groups containing
        // its own address) and on shared sub-constants.
        std::set<const Constant *> visited;
        std::function<void(const Constant *)> walkAny;
        walkAny = [&](const Constant *c) {
            if (!c) return;
            if (!visited.insert(c).second) return;
            // GlobalValue (Function/GlobalVariable) is terminal; don't
            // recurse into its operands (a GV's "operands" are its
            // initializer / aliasee — walking them traverses the entire
            // module graph and risks unbounded recursion).
            if (const auto *fn = dyn_cast<Function>(c)) {
                irqReachable.insert(fn->getName().str());
                return;
            }
            if (isa<GlobalValue>(c)) return;
            const Value *stripped = c->stripPointerCasts();
            if (const auto *fn = dyn_cast<Function>(stripped)) {
                irqReachable.insert(fn->getName().str());
                return;
            }
            for (unsigned i = 0, n = c->getNumOperands(); i < n; ++i)
                walkAny(dyn_cast<Constant>(c->getOperand(i)));
        };
        // Typed walk: descend into c as guided by t.  When t names an
        // async struct, switch to walkAny to capture its callback fields.
        std::function<void(const Constant *, Type *)> walkTyped;
        walkTyped = [&](const Constant *c, Type *t) {
            if (!c || !t) return;
            // GlobalValue at any level is terminal — never recurse through
            // a GV's initializer here (its parent GV is already iterated
            // by the M.globals() loop below).
            if (isa<GlobalValue>(c)) return;
            if (auto *ST = dyn_cast<StructType>(t)) {
                if (ST->hasName() && isAsyncStructName(ST->getName())) {
                    walkAny(c);
                    return;
                }
                if (const auto *CS = dyn_cast<ConstantStruct>(c)) {
                    for (unsigned i = 0, n = ST->getNumElements(); i < n; ++i)
                        walkTyped(CS->getOperand(i), ST->getElementType(i));
                }
                // ConstantAggregateZero / Undef: no function pointers.
                return;
            }
            if (auto *AT = dyn_cast<ArrayType>(t)) {
                if (const auto *CA = dyn_cast<ConstantArray>(c)) {
                    for (unsigned i = 0, n = CA->getNumOperands(); i < n; ++i)
                        walkTyped(cast<Constant>(CA->getOperand(i)),
                                  AT->getElementType());
                }
                return;
            }
        };
        for (const GlobalVariable &GV : M.globals()) {
            if (!GV.hasInitializer()) continue;
            walkTyped(GV.getInitializer(), GV.getValueType());
        }
    }

    // G5.2: propagate IRQ-reachable transitively.  Any function called

    // G5.2: propagate IRQ-reachable transitively.  Any function called
    // (directly) from an irq-reachable function is itself irq-reachable.
    // We reuse the `callers` map computed above (callee → callers), but
    // we want the forward direction (caller → callees) — build it inline.
    {
        std::map<std::string, std::set<std::string>> callees;
        for (Function &F : M) {
            if (F.isDeclaration()) continue;
            const std::string fname = F.getName().str();
            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {
                    const auto *CI = dyn_cast<CallInst>(&I);
                    if (!CI) continue;
                    const Function *cf = CI->getCalledFunction();
                    if (!cf || cf->isDeclaration()) continue;
                    callees[fname].insert(cf->getName().str());
                }
            }
        }
        std::queue<std::string> worklist;
        for (const auto &n : irqReachable) worklist.push(n);
        while (!worklist.empty()) {
            std::string n = worklist.front(); worklist.pop();
            auto it = callees.find(n);
            if (it == callees.end()) continue;
            for (const auto &c : it->second)
                if (irqReachable.insert(c).second)
                    worklist.push(c);
        }
    }

    // ── G5.5: address-taken async-callback exemption (deferred) ───────────
    //
    // After irqReachable closure is computed, any function that
    //   (a) has its address taken (stored in a vtable / passed by pointer), AND
    //   (b) is reachable from a known async-context entry point
    //       (work / timer / IRQ / tasklet / kthread / irq_work — seeded
    //        from request_irq / INIT_WORK / tasklet_init / kthread_create /
    //        init_irq_work and propagated transitively above),
    // is a runtime callback that the kernel can invoke INDEPENDENTLY of any
    // init/exit lifecycle phase.  Strip it from initOnly/exitOnly/
    // moduleInitOnly/moduleExitOnly so H27 cannot suppress real races.
    //
    // The irqReachable gate is critical: address-taken alone is too broad.
    // Many driver subsystems (psmouse, kvm-vfio, console drivers) store
    // probe/disconnect callbacks in protocol vtables; those address-taken
    // functions are invoked synchronously from probe()/remove() and remain
    // serialised by the driver-model device_lock.  Only address-taken funcs
    // that the kernel invokes asynchronously via a registered async context
    // can legitimately race with init/exit code, and irqReachable is the
    // soundness witness for "invoked from an async context".
    //
    // Exclusion: do NOT apply if the function is itself a section/name-tagged
    // init/exit (init_module, *_probe, *_kref_release).  Such functions may
    // have their address taken by the module loader's __initcall table.
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        if (!F.hasAddressTaken()) continue;
        const std::string fname = F.getName().str();
        StringRef sec = F.getSection();
        if (sec.contains(".init.text") || sec.contains(".exit.text")) continue;
        if (isInitName(F.getName()) || isExitName(F.getName())) continue;
        sysfsRuntime.insert(fname);
        moduleInitOnly.erase(fname);
        moduleExitOnly.erase(fname);
        initOnly.erase(fname);
        exitOnly.erase(fname);
    }

    // ── 3. Rebuild SVF for PTA-based analyses ─────────────────────────────
    const_cast<Option<bool> &>(Options::PStat).setValue(false);
    normaliseKernelAllocators(M);

    LLVMModuleSet::buildSVFModule(M);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();
    auto *pta   = AndersenWaveDiff::createAndersenWaveDiff(pag);
    auto *msSet = LLVMModuleSet::getLLVMModuleSet();

    auto getBaseObjIds = [&](const Value *V) -> std::set<unsigned> {
        std::set<unsigned> ids;
        if (!V || !V->getType()->isPointerTy())
            return ids;
        if (!msSet->hasValueNode(V))
            return ids;
        NodeID nid = msSet->getValueNode(V);
        const PointsTo &pts = pta->getPts(nid);
        for (NodeID obj : pts) {
            if (const auto *gep = dyn_cast<GepObjVar>(pag->getGNode(obj)))
                ids.insert(gep->getBaseObj()->getId());
            else
                ids.insert(obj);
        }
        return ids;
    };

    // H2/H7/H9/H10: exclusive lock sets (arg0 = lock pointer)
    std::map<std::string, std::set<unsigned>> lockSets;
    // NodeID → global variable name for LockAlias sub-category reporting
    std::map<unsigned, std::string> lockNames;
    // H14: completion signal / wait (arg0 = completion pointer)
    std::map<std::string, std::set<unsigned>> completionSignal, completionWait;
    // H8: per-CPU variable sets (arg0 = percpu pointer)
    std::map<std::string, std::set<unsigned>> perCpuSets;

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        const std::string fname = F.getName().str();
        auto &ls  = lockSets[fname];
        auto &sig = completionSignal[fname];
        auto &wt  = completionWait[fname];
        auto &pc  = perCpuSets[fname];

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                const auto *CI = dyn_cast<CallInst>(&I);
                if (!CI)
                    continue;

                // H_InlineASMLock: inline-ASM spinlock patterns (AArch64 pKVM
                // and similar). ldadda / ldaddal are atomic-add lock-acquire;
                // ldaxr+stxr is the LL/SC spinlock loop.  Detect either form
                // and add every pointer-typed argument's base objects to the
                // function's lock set so LockAliasHeuristic can match them.
                //
                // Safety: only pointer args pointing to global variables are
                // added (getBaseObjIds filters non-pointer / non-global values
                // through SVF PTA), so spurious widening is minimal.
                if (CI->isInlineAsm()) {
                    if (const auto *IA =
                            dyn_cast<InlineAsm>(CI->getCalledOperand())) {
                        const std::string &asmStr = IA->getAsmString();
                        bool hasLdadda =
                            asmStr.find("ldadda")  != std::string::npos ||
                            asmStr.find("ldaddal") != std::string::npos;
                        bool hasLdaxr =
                            asmStr.find("ldaxr") != std::string::npos &&
                            asmStr.find("stxr")  != std::string::npos;
                        if (hasLdadda || hasLdaxr) {
                            for (unsigned i = 0; i < CI->arg_size(); ++i) {
                                const Value *arg = CI->getArgOperand(i);
                                if (!arg->getType()->isPointerTy())
                                    continue;
                                for (unsigned id : getBaseObjIds(arg))
                                    ls.insert(id);
                            }
                        }
                    }
                    continue; // no Function* to query below
                }

                const Function *callee = CI->getCalledFunction();
                if (!callee)
                    continue;
                StringRef cn = callee->getName();

                // H2/H7/H9/H10: exclusive lock acquire — arg0 is lock ptr
                if (isExclusiveLockAcquire(cn) && CI->arg_size() > 0) {
                    const Value *lockArg = CI->getArgOperand(0);
                    for (unsigned id : getBaseObjIds(lockArg)) {
                        ls.insert(id);
                        // Record global variable name for sub-category reporting.
                        // Strip GEPs / bitcasts to find the underlying GlobalVariable.
                        if (lockNames.find(id) == lockNames.end()) {
                            const Value *base = lockArg->stripPointerCasts();
                            if (const auto *GV = dyn_cast<GlobalVariable>(base))
                                lockNames[id] = ("@" + GV->getName()).str();
                        }
                    }
                }

                // H14: completion signal — arg0 is completion ptr
                if (isCompletionSignal(cn) && CI->arg_size() > 0)
                    for (unsigned id : getBaseObjIds(CI->getArgOperand(0)))
                        sig.insert(id);

                // H14: completion wait — arg0 is completion ptr
                if (isCompletionWait(cn) && CI->arg_size() > 0)
                    for (unsigned id : getBaseObjIds(CI->getArgOperand(0)))
                        wt.insert(id);

                // H8: per-CPU access — arg0 is percpu var ptr
                if ((cn == "get_cpu_ptr" || cn == "__get_cpu_ptr" ||
                     cn == "raw_cpu_ptr" || cn == "__this_cpu_ptr") &&
                    CI->arg_size() > 0)
                    for (unsigned id : getBaseObjIds(CI->getArgOperand(0)))
                        pc.insert(id);
            }
        }
    }

    // ── 4. Lock inheritance propagation (H_LockProp) ───────────────────────
    //
    // Propagate lock sets through call chains so that functions which are
    // always called under a lock — but do not call the lock acquire function
    // themselves — are also considered lock-holders.
    //
    // Algorithm:
    //   1. Build a complete callee→callers map (all functions in module,
    //      direct call edges only — not function-pointer indirect calls).
    //   2. Fixpoint: for each non-externally-callable function F that does
    //      not yet hold lock L,
    //        if ALL direct intra-module callers of F hold L:
    //          add L to F's lock set.
    //   3. Repeat until stable.
    //
    // Soundness:
    //   - `propagatable` restricts F to internal/private/hidden-visibility
    //     functions, which by linkage rules can ONLY be called from within
    //     this translation unit.  Therefore every caller of F is visible in
    //     this module; the "all callers hold L" intersection is exhaustive.
    //   - The lock identity is the SVF PTA base-object NodeID.  PTA
    //     distinguishes per-allocation-site heap objects, so propagating a
    //     field-lock (e.g. `inode->i_lock`) is safe: every caller in this
    //     module that holds `inode->i_lock` holds it on the SAME inode
    //     object PTA already conflated together.
    //
    // Previously this propagation was restricted to globals with internal
    // linkage (a stricter soundness argument).  That restriction prevented
    // propagation of struct-field locks (the dominant case in driver code
    // where every entry point takes `port->lock` or `inode->i_lock` before
    // calling helpers), causing the framework to miss large classes of
    // legitimate lock-protected pairs.  The restriction is removed: as long
    // as F is non-externally-callable, the inference is sound regardless of
    // whether the lock is a global or a field of a PTA-tracked object.

    // Step 4a: build callee → set-of-callers map.
    std::map<std::string, std::set<std::string>> lockPropCallers;
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                const auto *CI = dyn_cast<CallInst>(&I);
                if (!CI) continue;
                const Function *callee = CI->getCalledFunction();
                if (!callee || callee->isDeclaration()) continue;
                lockPropCallers[callee->getName().str()]
                    .insert(F.getName().str());
            }
        }
    }

    // Step 4b: fixpoint propagation.
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (Function &F : M) {
                if (F.isDeclaration())
                    continue;
                // Only propagate INTO internal/private/hidden functions.
                // External-linkage functions may be called from any context.
                bool propagatable =
                    F.hasInternalLinkage() || F.hasPrivateLinkage() ||
                    F.hasHiddenVisibility();
                if (!propagatable)
                    continue;

                const std::string fname = F.getName().str();
                auto &ls = lockSets[fname];

                const auto &callerNames = lockPropCallers[fname];
                if (callerNames.empty())
                    continue; // no intra-module callers found — skip

                // Intersection of all callers' lock sets.
                std::set<unsigned> common;
                bool first = true;
                for (const auto &cr : callerNames) {
                    auto it = lockSets.find(cr);
                    if (it == lockSets.end() || it->second.empty()) {
                        common.clear();
                        break; // a caller holds no locks → no common locks
                    }
                    if (first) {
                        common = it->second;
                        first = false;
                    } else {
                        std::set<unsigned> next;
                        for (unsigned id : common)
                            if (it->second.count(id))
                                next.insert(id);
                        common = std::move(next);
                    }
                    if (common.empty())
                        break;
                }

                // Add any newly-inferred locks.
                for (unsigned id : common) {
                    if (!ls.count(id)) {
                        ls.insert(id);
                        changed = true;
                    }
                }
            }
        }
    }

    // ── 5. Teardown SVF ───────────────────────────────────────────────────
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    NodeIDAllocator::unset();

    // ── 6. Assemble heuristics (M1–M9 + Conservative) ────────────────────
    std::vector<std::unique_ptr<PairHeuristic>> heuristics;

    // H_BA: BenignAnchor — suppress pairs whose anchor is a known-benign
    // non-ordering call (WARN_ONCE, idempotent once-write flags).
    // Registered first: pure string comparison, zero cost.
    heuristics.push_back(std::make_unique<BenignAnchorHeuristic>());
    // H_RR: Read-Read — cheapest filter; no writes on either side → no race.
    // Must be second: eliminates ~67% of pairs at near-zero cost.
    heuristics.push_back(std::make_unique<ReadReadPair>());
    // M1: H1 — call-graph sequential / common caller (with G5.2 IRQ-context guard)
    heuristics.push_back(std::make_unique<CallGraphHeuristic>(
        std::move(irqReachable)));
    // M2: H2 + H7 + H9 + H10 — same exclusive lock (PTA-resolved)
    heuristics.push_back(std::make_unique<LockAliasHeuristic>(
        std::move(lockSets), std::move(lockNames)));
    // M3: H3 + H16 — RCU / SRCU grace-period ordering
    heuristics.push_back(std::make_unique<RcuSerialHeuristic>(
        std::move(rcuReaders), std::move(rcuSync)));
    // Seqlock writer vs retrying reader: safety is the retry, not ordering.
    heuristics.push_back(std::make_unique<SeqlockRetryHeuristic>(
        std::move(seqWriters), std::move(seqReaders)));
    // M4: H4 + H20 — IRQ / BH disable serialization
    heuristics.push_back(std::make_unique<IrqSerialHeuristic>(
        std::move(irqDisablers), std::move(irqSyncCallers)));
    // M5 + M6: H5 + H6 + H12 + H17 + H19 + H23 + H27 — lifecycle phase separation
    heuristics.push_back(std::make_unique<LifecycleHeuristic>(
        std::move(initOnly), std::move(exitOnly),
        std::move(moduleInitOnly), std::move(moduleExitOnly)));
    // M7: H8 — per-CPU variable (no cross-CPU sharing)
    heuristics.push_back(std::make_unique<PerCpuHeuristic>(
        std::move(perCpuSets)));
    // M8: H14 — kernel completion signal/wait ordering
    heuristics.push_back(std::make_unique<CompletionOrderHeuristic>(
        std::move(completionSignal), std::move(completionWait)));
    // M2 extension: H13 — zero-arg subsystem locks (rtnl_lock etc.)
    heuristics.push_back(std::make_unique<SubsystemLockHeuristic>(
        std::move(subsysLocks)));
    // M9: H11 + H15 + H21 + H24 + H25 — safe / benign allocation access
    heuristics.push_back(std::make_unique<SafeAllocationHeuristic>(
        std::move(regmapFns), std::move(dmaSyncFns),
        std::move(seqlockRdFns), std::move(seqlockWrFns)));
    // H2p (ConservativeAnyLock) is intentionally NOT added here.
    // "Both fns hold some exclusive lock" does not imply mutual exclusion
    // unless they hold the *same* lock — which is already handled precisely
    // by LockAliasHeuristic (H2/H7/H9/H10) with PTA.  Enabling H2p causes
    // false negatives whenever fn1 and fn2 hold *different* locks.

    // ── 7. Filter ─────────────────────────────────────────────────────────
    const auto &disabled = disabledHeuristics();
    for (const auto &[anchorFn, pairList] : pairs.byAnchorFn) {
        for (const FunctionPair &p : pairList) {
            ++result.totalPairs;
            bool filtered = false;
            for (const auto &h : heuristics) {
                std::string subcat = h->isMutuallyExclusive(p, M, MAM);
                if (!subcat.empty()) {
                    // Always record the hit so we can quantify what each
                    // heuristic would have filtered.  But honour the ablation
                    // set: if this heuristic is disabled, do NOT actually
                    // filter the pair — let it flow through to litmus
                    // generation so we can measure hidden TPs.
                    ++result.heuristicHits[h->name()];
                    ++result.heuristicSubHits[h->name()][subcat];
                    if (disabled.count(h->name())) {
                        // Skip this heuristic's verdict; keep evaluating
                        // the remaining heuristics so a pair is filtered
                        // only if some enabled heuristic still matches.
                        continue;
                    }
                    result.filteredRecords.push_back({p, h->name(), subcat});
                    filtered = true;
                    break;
                }
            }
            if (!filtered)
                result.byAnchorFn[anchorFn].push_back(p);
        }
    }

    return result;
}

