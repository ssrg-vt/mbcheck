#include "lib/phase_timer.h"
#include "lib/anchor_pass.h"
#include "lib/anchors.h"
#include "lib/ext_fn_summary.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace llvm;
using namespace anchors;

AnalysisKey AnchorPass::Key;

// ---------------------------------------------------------------------------
// Inline-asm string helpers
// ---------------------------------------------------------------------------

/// Decode LLVM IR inline-asm hex escapes: \XX → char(0xXX).
/// \09 (HT) and \0A (LF) are the most common; all two-hex-digit sequences
/// are handled generically.
static std::string unescapeAsm(StringRef s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) {
            auto hexDigit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int hi = hexDigit(s[i + 1]), lo = hexDigit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

/// Lower-case every character in s.
static std::string toLower(const std::string &s)
{
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
        r += static_cast<char>(std::tolower(c));
    return r;
}

/// Scan one logical line of asm text (after unescaping) for a known mnemonic.
/// Returns a pointer into kAsmAnchors, or nullptr.
///
/// Matching strategy:
///   • If the first word is "dmb" or "dsb" (instructions that take a
///     domain/type qualifier as their sole operand), attempt a two-word
///     match ("dmb ish", "dmb ishst", …) using the qualifier as word1.
///   • Otherwise attempt a one-word exact match against kAsmAnchors.
///
/// "Word" = maximal run of non-whitespace, non-comma characters.
static const AsmAnchor *matchMnemonic(const std::string &word0,
                                      const std::string &word1)
{
    if ((word0 == "dmb" || word0 == "dsb") && !word1.empty()) {
        // Qualifier may carry a trailing comma from some assembler dialects;
        // strip it for matching.
        std::string q = word1;
        if (!q.empty() && q.back() == ',')
            q.pop_back();
        std::string combo = word0 + " " + q;
        for (const auto &a : kAsmAnchors)
            if (a.mnemonic == combo)
                return &a;
        return nullptr; // unknown dmb/dsb barrier type — not in our table
    }
    for (const auto &a : kAsmAnchors)
        if (a.mnemonic == word0)
            return &a;
    return nullptr;
}

/// Parse an inline-asm string and collect all matching AsmAnchor entries.
/// Results are deduplicated (same label reported at most once per call).
static void collectAsmAnchors(StringRef asmStr,
                               SmallVectorImpl<const AsmAnchor *> &out)
{
    std::string text = unescapeAsm(asmStr);
    std::set<std::string_view> seen;

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip leading whitespace.
        size_t s0 = line.find_first_not_of(" \t");
        if (s0 == std::string::npos)
            continue;
        line = line.substr(s0);
        if (line.empty())
            continue;

        // Skip assembler directives, C/C++ comments, preprocessor lines.
        char first = line[0];
        if (first == '.' || first == '/' || first == '#')
            continue;

        // Skip numeric labels (e.g. "661:", "1:") used by altinstruction blobs.
        if (std::isdigit(static_cast<unsigned char>(first)))
            continue;

        // Skip named labels: any token ending with ':' that comes before the
        // first space (e.g. "myLabel:", "__asm_label:").
        {
            size_t colon = line.find(':');
            size_t space = line.find_first_of(" \t");
            if (colon != std::string::npos &&
                (space == std::string::npos || colon < space))
                continue;
        }

        // Extract up to the first two whitespace/comma-separated tokens,
        // converted to lower-case.
        auto nextTok = [&](const std::string &l, size_t &pos) -> std::string {
            size_t start = l.find_first_not_of(" \t", pos);
            if (start == std::string::npos) {
                pos = l.size();
                return {};
            }
            size_t end = l.find_first_of(" \t,", start);
            pos = (end == std::string::npos) ? l.size() : end;
            return toLower(l.substr(start, pos - start));
        };

        size_t pos = 0;
        std::string w0 = nextTok(line, pos);
        std::string w1 = nextTok(line, pos);

        const AsmAnchor *hit = matchMnemonic(w0, w1);
        if (hit && seen.insert(hit->label).second)
            out.push_back(hit);
    }
}

// ---------------------------------------------------------------------------
// External-function name matching
// ---------------------------------------------------------------------------

/// Longest-prefix match of a declared function name against kExtAnchors.
/// Returns nullptr if no entry matches.
static const ExtAnchor *matchExtFn(StringRef name)
{
    std::string low = toLower(name.str());
    const ExtAnchor *best = nullptr;
    size_t bestLen = 0;
    for (const auto &ea : kExtAnchors) {
        if (low.size() >= ea.prefix.size() &&
            low.compare(0, ea.prefix.size(), ea.prefix) == 0 &&
            ea.prefix.size() > bestLen) {
            best = &ea;
            bestLen = ea.prefix.size();
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// kExtFnSummary hashtable — built once at first call for O(1) exact-name lookup
// ---------------------------------------------------------------------------

/// Returns a reference to a lazily-initialised unordered_map from exact
/// lower-case function name → pointer into kExtFnSummary.
static const std::unordered_map<std::string_view, const ExtFnSummary *> &
extFnSummaryTable()
{
    static const auto map = []() {
        std::unordered_map<std::string_view, const ExtFnSummary *> m;
        m.reserve(kExtFnSummary.size());
        for (const auto &e : kExtFnSummary)
            m.emplace(e.name, &e);
        return m;
    }();
    return map;
}

// ---------------------------------------------------------------------------
// Helper: obtain InlineAsm* from a call-like instruction (CallInst or
// CallBrInst — the latter is used by Linux's "asm goto" / alternative blobs).
// ---------------------------------------------------------------------------
static InlineAsm *getInlineAsm(Instruction &I)
{
    if (auto *CI = dyn_cast<CallInst>(&I))
        return dyn_cast<InlineAsm>(CI->getCalledOperand());
    if (auto *CBR = dyn_cast<CallBrInst>(&I))
        return dyn_cast<InlineAsm>(CBR->getCalledOperand());
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pass implementation
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Value-flow: trace a pointer back to its definition origin.
//
// Used by computeFnEffectSummary to decide whether an anchor's memory
// operand reflects state that the CALLER might observe (arg-driven) versus
// state internal to the callee (a global like log-buffer head, or a stack
// alloca).  This is the principled fix for the FP cluster discovered in the
// 3160-file sweep where helpers like print_sys_reg_msg propagated phantom SC
// effect to all callers because their internal log-buffer RMW was treated as
// a synchronization primitive.
//
// The trace walks SSA def-use chains backward through pointer-arithmetic
// (GEP), bitcasts, addrspace casts, PHIs, selects, and int<->ptr casts.
// Loads from arbitrary pointers are not followed (we'd need full alias
// analysis); they yield `Unknown`, which is treated conservatively as
// "possibly arg-driven" by the caller.
// ---------------------------------------------------------------------------
enum class PointerOrigin {
    Argument,   // reaches at least one Argument
    Internal,   // reaches only GlobalValues and/or AllocaInsts
    Unknown,    // gave up (depth limit, unsupported instruction, load, etc.)
};

static PointerOrigin tracePointerOrigin(const Value *V)
{
    SmallPtrSet<const Value *, 16> seen;
    SmallVector<const Value *, 8> stk;
    stk.push_back(V);
    bool sawArg = false;
    bool sawInternal = false;
    int budget = 64;  // bound to keep this fast on huge IR
    while (!stk.empty() && budget-- > 0) {
        const Value *v = stk.back();
        stk.pop_back();
        if (!seen.insert(v).second) continue;
        v = v->stripPointerCasts();
        if (!seen.insert(v).second) continue;

        if (isa<Argument>(v))     { sawArg = true; continue; }
        if (auto *GV = dyn_cast<GlobalValue>(v)) {
            // T2.2 refinement: an exported (non-local-linkage) MUTABLE global
            // is genuinely caller-visible — readers in other translation
            // units can observe its updates, so an anchor on it must order
            // those readers w.r.t. concurrent writers.  Only treat
            // local-linkage / constant globals as callee-internal state.
            bool isExportedMutable = !GV->hasLocalLinkage();
            if (auto *GVar = dyn_cast<GlobalVariable>(GV))
                isExportedMutable = isExportedMutable && !GVar->isConstant();
            if (isExportedMutable) { sawArg = true; continue; }
            sawInternal = true; continue;
        }
        if (isa<AllocaInst>(v))   { sawInternal = true; continue; }
        if (isa<Constant>(v))     { sawInternal = true; continue; }

        if (auto *GEP = dyn_cast<GetElementPtrInst>(v)) {
            stk.push_back(GEP->getPointerOperand());
            continue;
        }
        if (auto *BC = dyn_cast<BitCastInst>(v)) {
            stk.push_back(BC->getOperand(0));
            continue;
        }
        if (auto *AC = dyn_cast<AddrSpaceCastInst>(v)) {
            stk.push_back(AC->getOperand(0));
            continue;
        }
        if (auto *PHI = dyn_cast<PHINode>(v)) {
            for (unsigned i = 0, n = PHI->getNumIncomingValues(); i < n; ++i)
                stk.push_back(PHI->getIncomingValue(i));
            continue;
        }
        if (auto *Sel = dyn_cast<SelectInst>(v)) {
            stk.push_back(Sel->getTrueValue());
            stk.push_back(Sel->getFalseValue());
            continue;
        }
        if (auto *I2P = dyn_cast<IntToPtrInst>(v)) {
            stk.push_back(I2P->getOperand(0));
            continue;
        }
        if (auto *P2I = dyn_cast<PtrToIntInst>(v)) {
            stk.push_back(P2I->getOperand(0));
            continue;
        }
        // Load, Call, binop, etc. — origin is opaque without alias analysis.
        return PointerOrigin::Unknown;
    }
    if (budget <= 0) return PointerOrigin::Unknown;
    if (sawArg) return PointerOrigin::Argument;
    if (sawInternal) return PointerOrigin::Internal;
    return PointerOrigin::Unknown;
}

// ---------------------------------------------------------------------------
// Decide whether an anchor instruction's memory operand(s) make it
// CALLER-VISIBLE — i.e. it touches memory the caller could in principle
// observe (caller-supplied pointer or its derivatives).
//
// Policy (conservative — INCLUDE unless we can prove internal):
//   • Anchor has zero pointer-typed operands (pure fence like DMB ISH,
//     synchronize_rcu, etc.)                            → INCLUDE
//   • At least one pointer operand traces to an Argument or Unknown
//                                                       → INCLUDE
//   • ALL pointer operands trace to Internal (Global / Alloca / Constant)
//                                                       → EXCLUDE
// ---------------------------------------------------------------------------
static bool anchorIsCallerVisible(const CallBase &CB)
{
    bool sawPointer = false;
    bool allInternal = true;
    for (const Value *arg : CB.args()) {
        if (!arg->getType()->isPointerTy()) continue;
        sawPointer = true;
        PointerOrigin o = tracePointerOrigin(arg);
        if (o != PointerOrigin::Internal) {
            allInternal = false;
            break;
        }
    }
    if (!sawPointer) return true;       // pure fence
    return !allInternal;
}

// ---------------------------------------------------------------------------
// Decide whether a call from caller F to callee G should propagate G's
// caller-visible effect to F.  We require that at least one pointer-typed
// argument of the F→G call site SSA-traces back to a parameter of F (or is
// of Unknown origin — conservative).  If all pointer args of the call site
// originate from F-internal state (F's allocas, globals, or constants), G's
// effect operates only on F-internal memory and is therefore not visible to
// F's callers either.
// ---------------------------------------------------------------------------
static bool callPropagatesEffect(const CallBase &CB)
{
    bool sawPointer = false;
    bool allInternal = true;
    for (const Value *arg : CB.args()) {
        if (!arg->getType()->isPointerTy()) continue;
        sawPointer = true;
        PointerOrigin o = tracePointerOrigin(arg);
        if (o != PointerOrigin::Internal) {
            allInternal = false;
            break;
        }
    }
    if (!sawPointer) return true;       // pure fence — propagates globally
    return !allInternal;
}

// ---------------------------------------------------------------------------
// Helper: classify an external-fn call's memory-ordering effect.
// Returns the strongest Ordering produced by the call site (Acquire/Release/SC)
// or Ordering::None if the call is an anchor blocklist member or has no entry.
// ---------------------------------------------------------------------------
static Ordering externalCallEffect(const Function *callee)
{
    if (!callee || !callee->isDeclaration() || callee->isIntrinsic())
        return Ordering::None;
    if (const ExtAnchor *ea = matchExtFn(callee->getName()))
        return ea->ordering;
    std::string low = toLower(callee->getName().str());
    if (anchors::isBlocklistedExtAnchor(low))
        return Ordering::None;
    const auto &tbl = extFnSummaryTable();
    auto it = tbl.find(std::string_view(low));
    if (it == tbl.end())
        return Ordering::None;
    return it->second->ordering;
}

// ---------------------------------------------------------------------------
// Helper: is this internal-fn name a utility helper whose internal atomic
// ops / fences DO NOT provide memory ordering for the caller's data?
//
// Examples and rationale:
//   • print_*, *_print, *_dump, *_show, *_store          — printk family;
//     printk internally uses SC RMW on the log-buffer ringhead, but that
//     synchronizes only printk readers, not the caller's data.
//   • dev_info/warn/err/dbg/emerg/alert/crit/notice      — dev_printk family.
//   • WARN*, BUG*, panic*, __warn*, __ubsan_*, __asan_*, kasan_*, kcsan_*,
//     kmsan_*, kcov_*                                    — diagnostic /
//     sanitizer instrumentation; injects atomics for its own bookkeeping.
//   • trace_*, *_trace, ftrace_*, __ftrace_*             — tracepoint
//     bookkeeping (ringbuffer head update is SC RMW).
//   • add_taint, set_bit on global flags                 — flag updates;
//     no payload synchronization relationship.
//
// Returning true here means the call-effect summary will NOT propagate this
// function's effect to its callers AND the rule-3 anchor emission will skip
// generating a (call-effect) anchor for calls to this function.
// Internal anchors INSIDE the function body are still emitted normally for
// litmus tests where this function is itself an anchor-fn.
// ---------------------------------------------------------------------------
static bool isUtilityHelperName(llvm::StringRef rawName)
{
    // Strip LLVM disambiguation suffix (e.g. "print_sys_reg_msg.392").
    auto dot = rawName.find('.');
    llvm::StringRef name = (dot == llvm::StringRef::npos)
                               ? rawName
                               : rawName.substr(0, dot);
    std::string low = toLower(name.str());

    // ---- substring patterns (most general) ----
    static const char *kSubstrings[] = {
        "print",      // print_*, *_print, sprint, vprint, printk
        "dump",       // dump_*, *_dump, dump_stack
        "_show",      // sysfs read
        "_store",     // sysfs write
        "trace",      // trace_*, ftrace_*, *_trace
        "warn",       // WARN, __warn, pr_warn, dev_warn
        "panic",
        "kasan",
        "kcsan",
        "kmsan",
        "kcov",
        "ubsan",
        "__sanitizer",
        "__asan",
        "__msan",
        "__ubsan",
        "syslog",
        "klog",
    };
    for (const char *s : kSubstrings) {
        if (low.find(s) != std::string::npos) return true;
    }

    // ---- prefix patterns ----
    static const char *kPrefixes[] = {
        "pr_",            // pr_info / pr_err / pr_warn / pr_debug / pr_cont
        "dev_",           // dev_info / dev_warn / dev_err / dev_dbg / ...
        "netdev_",        // netdev_info / netdev_warn / netdev_err
        // KVM fault-injection / state-mutator helpers: their internal SC
        // RMWs (e.g. vcpu_kick → wait-queue) synchronize the VCPU thread,
        // not the trap-handler caller's data.  Empirically the largest FP
        // cluster in the 3160-file sweep.
        "kvm_inject_",
        "kvm_pmu_",
        "kvm_debug_",
        "kvm_arm_timer_",
        "kvm_vcpu_pmu_",
        // Audit helpers — internal audit-context locks, not caller payload.
        // From v8 ext_fn_summary audit: ~80 audit_* / __audit_* fns recorded
        // as SC because internal audit_buffer locking uses ldaddal.
        "__audit_",
        "audit_",
        // Bitmap operations on caller-supplied buffers — any internal SC
        // anchors are KASAN/KCSAN probes, not synchronization of caller data.
        "__bitmap_",
        // Ratelimit bookkeeping — internal lock on the ratelimit_state, not
        // on the caller's data being rate-limited.
        "___ratelimit",
        "__ratelimit",
    };
    for (const char *s : kPrefixes) {
        if (low.rfind(s, 0) == 0) return true;
    }

    // ---- exact-name list of common taint/flag / state-mutator helpers ----
    static const char *kExacts[] = {
        "add_taint",
        "test_taint",
        "vcpu_write_sys_reg",
        "vcpu_read_sys_reg",
        "read_sanitised_ftr_reg",
        "panic_print_sys_info",
        "show_regs",
        "stack_trace_save",
        "stack_trace_save_user",
        "save_stack_trace",
        "dump_stack",
        "dump_stack_lvl",
        // User-access protections (SMAP/PAN toggles use dmb/dsb but the
        // ordering is for the access-permission register, not the caller's
        // payload buffer being copied).
        "__arch_copy_to_user",
        "__arch_copy_from_user",
        "__arch_clear_user",
    };
    for (const char *s : kExacts) {
        if (low == s) return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Compute per-function call-effect summary (G8, ask #1).
//
// For each DEFINED function F in the module compute fnEffect[F] = the
// strongest hardware-visible ordering reachable inside F via any of:
//   • inline-asm anchors          (kAsmAnchors)
//   • volatile stores              (WRITE_ONCE)
//   • calls to declared functions  (kExtAnchors / kExtFnSummary)
//   • calls to OTHER defined fns   (transitively — fixpoint)
//
// The recursive case is what closes the gap: when a non-anchor function F1
// contains only a call to a non-anchor helper F2 which itself eventually
// calls smp_store_release, the call F1→F2 in F2's caller now contributes an
// anchor.  Without this, the framework would miss the ordering whenever the
// programmer factored synchronization into a helper function.
//
// Effect strength uses the numeric Ordering enum (higher = stronger);
// CompilerOnly and Relaxed contribute None for anchor purposes.
//
// Fixpoint terminates because the lattice has finite height (5) and is
// monotonically increasing under join.  Worst-case complexity O(N · E).
// ---------------------------------------------------------------------------
static std::map<const Function *, Ordering>
computeFnEffectSummary(Module &M)
{
    std::map<const Function *, Ordering> effect;

    auto raiseTo = [](Ordering &cur, Ordering n) {
        // Compiler-only / relaxed are ignored for anchor effect purposes.
        if (n == Ordering::CompilerOnly || n == Ordering::Relaxed ||
            n == Ordering::None)
            return false;
        if (static_cast<uint8_t>(n) > static_cast<uint8_t>(cur)) {
            cur = n;
            return true;
        }
        return false;
    };

    // Seed: scan every function's body for direct anchors only.
    //
    // Value-flow gating (G8-VF): an anchor only contributes to F's effect
    // if its memory operand can be observed by F's callers — i.e. it
    // touches caller-supplied state (arg-derived) rather than callee-
    // internal state (globals/allocas).  This eliminates the
    // print/log/tracing/taint FP family at the structural level: their
    // anchors operate on a callee-internal log-buffer head or taint bitmap,
    // never on memory the caller is racing on.
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        // Utility-helper name blocklist remains as a second line of defense
        // for cases value-flow cannot disprove (e.g. kvm_inject_undefined
        // mutates vcpu state derived from its arg, but that state is the
        // vcpu's own — not the trap-handler caller's data).
        if (isUtilityHelperName(F.getName()))
            continue;
        Ordering &e = effect[&F]; // default-constructs to Ordering::None
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                // Inline asm anchors.
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (auto *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
                        SmallVector<const AsmAnchor *, 4> asmHits;
                        collectAsmAnchors(IA->getAsmString(), asmHits);
                        if (!asmHits.empty() && anchorIsCallerVisible(*CI))
                            for (const AsmAnchor *a : asmHits)
                                raiseTo(e, a->ordering);
                    }
                }
                if (auto *CBR = dyn_cast<CallBrInst>(&I)) {
                    if (auto *IA = dyn_cast<InlineAsm>(CBR->getCalledOperand())) {
                        SmallVector<const AsmAnchor *, 4> asmHits;
                        collectAsmAnchors(IA->getAsmString(), asmHits);
                        if (!asmHits.empty() && anchorIsCallerVisible(*CBR))
                            for (const AsmAnchor *a : asmHits)
                                raiseTo(e, a->ordering);
                    }
                }
                // Volatile store (WRITE_ONCE) — compiler-only, ignored.
                // External-fn calls.
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (Function *callee = CI->getCalledFunction()) {
                        Ordering ce = externalCallEffect(callee);
                        if (ce != Ordering::None &&
                            anchorIsCallerVisible(*CI))
                            raiseTo(e, ce);
                    }
                }
            }
        }
    }

    // Transitive propagation: F's effect = max(F's direct, max over callees G
    // such that the F→G call passes at least one F-arg-derived pointer (or no
    // pointers at all — pure fence).  Iterate to fixpoint.
    bool changed = true;
    while (changed) {
        changed = false;
        for (Function &F : M) {
            if (F.isDeclaration())
                continue;
            if (isUtilityHelperName(F.getName()))
                continue;
            Ordering &e = effect[&F];
            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {
                    const auto *CI = dyn_cast<CallInst>(&I);
                    if (!CI) continue;
                    const Function *callee = CI->getCalledFunction();
                    if (!callee || callee->isDeclaration()) continue;
                    // Block call-effect from utility helpers (see comment
                    // on isUtilityHelperName).
                    if (isUtilityHelperName(callee->getName())) continue;
                    auto it = effect.find(callee);
                    if (it == effect.end()) continue;
                    if (it->second == Ordering::None) continue;
                    // Value-flow gate: only inherit G's effect if THIS call
                    // site passes F-arg-derived pointers (or no pointers).
                    if (!callPropagatesEffect(*CI)) continue;
                    if (raiseTo(e, it->second))
                        changed = true;
                }
            }
        }
    }

    return effect;
}

AnchorResult AnchorPass::run(Module &M, ModuleAnalysisManager &)
{
    mbtime::Scope _tAnchorDb("anchor_db");
    AnchorResult result;

    // Pre-compute per-function memory-effect summary so calls to internal
    // helpers can themselves be treated as anchors (ask #1 / G8).
    std::map<const Function *, Ordering> fnEffect;
    {
        mbtime::Scope _tSummary("summarize_extfn");
        fnEffect = computeFnEffectSummary(M);
    }

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        std::vector<AnchorHit> &hits =
            result.perFunction[F.getName().str()];
        std::set<std::string> seenLabels; // per-function deduplication

        auto addHit = [&](std::string label, Ordering ord,
                          std::string site, std::string asmInst = {}) {
            if (ord == Ordering::CompilerOnly)
                return; // compiler-only barriers carry no hardware ordering
            if (!seenLabels.insert(label).second)
                return; // already recorded for this function
            hits.push_back({std::move(label), ord, std::move(site),
                            std::move(asmInst)});
        };

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {

                // 1. Inline asm (CallInst or CallBrInst with InlineAsm callee).
                //    Only emit hits for:
                //      - ThreadBarrier (dmb/dsb): enforce ordering across all
                //        threads without being tied to a specific address.
                //      - Store / RMW: instructions that write to memory.
                //    Pure loads (ldar) and relaxed operations are skipped.
                if (InlineAsm *IA = getInlineAsm(I)) {
                    SmallVector<const AsmAnchor *, 4> asmHits;
                    collectAsmAnchors(IA->getAsmString(), asmHits);
                    for (const AsmAnchor *a : asmHits) {
                        if (a->kind == AnchorKind::Load)
                            continue; // pure load — no store, skip
                        if (a->ordering == Ordering::Relaxed)
                            continue; // relaxed — no ordering guarantee, skip
                        addHit(std::string(a->label), a->ordering,
                               "asm sideeffect");
                    }
                }

                // 2. Volatile store → WRITE_ONCE (a store to memory).
                //    Volatile loads (READ_ONCE) are pure loads — skipped.
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    if (SI->isVolatile())
                        addHit(std::string(kVolatileStoreLabel),
                               kVolatileStoreOrdering, "store volatile");
                }

                // 3. Call to a declared (external) non-intrinsic function.
                //    First try kExtAnchors (curated semantic prefixes, longest
                //    prefix wins).  If no prefix matches, fall back to an O(1)
                //    hashtable lookup in kExtFnSummary (empirical ordering data,
                //    10 280 exact names).
                //
                //    Names matched by anchors::isBlocklistedExtAnchor() are
                //    skipped entirely on the fallback path (callback-defer,
                //    pure utilities — see anchors.h for the rationale).  This
                //    prevents spurious AnchorBracket creation for functions
                //    whose recorded ordering is private to their own body and
                //    does NOT serialize the caller's shared data.
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (Function *callee = CI->getCalledFunction()) {
                        if (callee->isDeclaration() &&
                            !callee->isIntrinsic()) {
                            std::string name = callee->getName().str();
                            std::string site = "@" + name;
                            if (const ExtAnchor *ea =
                                    matchExtFn(callee->getName())) {
                                addHit(std::string(ea->label), ea->ordering,
                                       site);
                            } else {
                                std::string low = toLower(name);
                                if (anchors::isBlocklistedExtAnchor(low))
                                    continue;
                                const auto &tbl = extFnSummaryTable();
                                auto it = tbl.find(std::string_view(low));
                                if (it != tbl.end()) {
                                    const ExtFnSummary *es = it->second;
                                    addHit(low + " (ext)", es->ordering,
                                           site, std::string(es->asmInst));
                                }
                            }
                        } else if (!callee->isDeclaration() &&
                                   !callee->isIntrinsic()) {
                            // 4. Call to an internal (defined-in-this-module)
                            //    function with a non-trivial transitive
                            //    memory-ordering effect (ask #1 / G8).
                            //    Use the pre-computed fnEffect summary.
                            //    Skip utility helpers (print/log/debug/...)
                            //    even if their fnEffect happens to be set
                            //    (defense in depth — should already be None).
                            if (isUtilityHelperName(callee->getName()))
                                continue;
                            auto it = fnEffect.find(callee);
                            if (it != fnEffect.end()) {
                                Ordering e = it->second;
                                if (e == Ordering::Acquire ||
                                    e == Ordering::Release ||
                                    e == Ordering::SC) {
                                    std::string name = callee->getName().str();
                                    addHit(name + " (call-effect)", e,
                                           "@" + name);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Drop functions that turned out to have no anchors.
        if (hits.empty())
            result.perFunction.erase(F.getName().str());
    }

    return result;
}
