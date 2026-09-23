#include "lib/phase_timer.h"
#include "lib/herd7_transpiler.h"
#include "lib/fn_heuristics.h"
#include "lib/causal_detect.h"
#include "lib/anchor_pass.h"
#include "lib/anchors.h"
#include "lib/ext_fn_summary.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace llvm;
using namespace anchors;

AnalysisKey Herd7TranspilerPass::Key;

// ===========================================================================
// Ordering-instruction detection
// ===========================================================================
//
// Between the two causal shared-accesses in each function (before and after
// the anchor), we scan all intermediate instructions and collect any that
// carry a memory ordering guarantee — either as inline asm or as a call to an
// external function whose ordering is known from kExtAnchors / kExtFnSummary.
//
// Results are collected as a sequence of Herd7 instructions to insert between
// the shared-access register operations in the generated litmus thread.
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// Lazy O(1) lookup table for kExtFnSummary (same approach as anchor_pass.cpp)
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string_view, const ExtFnSummary *> &
extSummaryMap()
{
    static const auto m = []() {
        std::unordered_map<std::string_view, const ExtFnSummary *> t;
        t.reserve(kExtFnSummary.size());
        for (const auto &e : kExtFnSummary)
            t.emplace(e.name, &e);
        return t;
    }();
    return m;
}

// ---------------------------------------------------------------------------
// toLower helper
// ---------------------------------------------------------------------------
static std::string toLower(StringRef s)
{
    std::string r(s.str());
    for (char &c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// ---------------------------------------------------------------------------
// Decode LLVM IR inline-asm hex escapes (\09, \0A, etc.)
// ---------------------------------------------------------------------------
static std::string unescapeAsm(StringRef s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) {
            auto hd = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int hi = hd(s[i+1]), lo = hd(s[i+2]);
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

// ---------------------------------------------------------------------------
// OrderingInst — a single ordering instruction to insert into a litmus thread
// ---------------------------------------------------------------------------
struct OrderingInst {
    std::string herd7;         // herd7 asm token, e.g. "DMB ISH", "LDAR", "STLR"
    Ordering    ordering;      // strength
    bool        isLoadInsn  = false; // true for acquire-load mnemonics (ldar etc.)
    bool        isStoreInsn = false; // true for release-store mnemonics (stlr etc.)
};

// ---------------------------------------------------------------------------
// Barrier equivalent for a load/store ordering instruction.
//
// When an LDAR or STLR appears as an ordering instruction *between* two
// shared accesses (rather than as the explicit channel access), it cannot
// be emitted as a bare token in herd7 — it needs register operands that we
// don't have in that context.  We lower it to its pure-barrier equivalent:
//   LDAR  (acquire load)  → DMB LD
//   STLR  (release store) → DMB ST
//   other SC load/stores  → DMB ISH
// ---------------------------------------------------------------------------
static std::string barrierEquiv(const OrderingInst &oi)
{
    // Any load-acquire instruction (ldar, ldaxr, ldarb, casa, ldadda, ...) —
    // lower to pure barrier; SC variants need a full barrier, others just LD.
    if (oi.isLoadInsn)
        return (oi.ordering == Ordering::SC) ? "DMB ISH" : "DMB LD";
    // Any release-store instruction (stlr, stlxr, stlrb, casl, ldaddl, ...) —
    // lower to pure barrier; SC variants need a full barrier, others just ST.
    if (oi.isStoreInsn)
        return (oi.ordering == Ordering::SC) ? "DMB ISH" : "DMB ST";
    return oi.herd7; // already a pure barrier token (DMB ISH, DMB ST, etc.)
}

// ---------------------------------------------------------------------------
// Map an AArch64 mnemonic (from inline asm) to its canonical Herd7 upper-case
// form.  Two-word mnemonics like "dmb ish" are preserved as "DMB ISH".
// Returns empty string if the mnemonic has no meaningful ordering in herd7.
// ---------------------------------------------------------------------------
static std::string mnemonicToHerd7(const std::string &low)
{
    // Thread barriers (DMB/DSB)
    if (low == "dmb ish")   return "DMB ISH";
    if (low == "dmb ishst") return "DMB ST";
    if (low == "dmb ishld") return "DMB LD";
    if (low == "dmb osh")   return "DMB OSH";
    if (low == "dmb oshst") return "DMB ST";   // outer — closest herd7 form
    if (low == "dsb sy")    return "DSB SY";
    if (low == "dmb")       return "DMB ISH";  // bare dmb → full ISH
    if (low == "dsb")       return "DSB SY";
    // Acquire loads (word, byte, halfword; exclusive variants; RCPC)
    if (low == "ldar")      return "LDAR";
    if (low == "ldarb")     return "LDAR";
    if (low == "ldarh")     return "LDAR";
    if (low == "ldaxr")     return "LDAR";
    if (low == "ldaxrb")    return "LDAR";
    if (low == "ldaxrh")    return "LDAR";
    if (low == "ldapr")     return "LDAR";
    if (low == "ldaprb")    return "LDAR";
    if (low == "ldaprh")    return "LDAR";
    // Release stores (word, byte, halfword; exclusive variants)
    if (low == "stlr")      return "STLR";
    if (low == "stlrb")     return "STLR";
    if (low == "stlrh")     return "STLR";
    if (low == "stlxr")     return "STLR";
    if (low == "stlxrb")    return "STLR";
    if (low == "stlxrh")    return "STLR";
    // SC atomics (acquire+release)
    if (low == "casal")     return "CASAL";
    if (low == "ldaddal")   return "LDADDAL";
    if (low == "ldclral")   return "LDCLRAL";
    if (low == "ldeoral")   return "LDEORAL";
    if (low == "ldsetal")   return "LDSETAL";
    if (low == "swpal")     return "SWPAL";
    // Acquire-only atomics
    if (low == "ldadda")    return "LDADDA";
    if (low == "ldclra")    return "LDCLRA";
    if (low == "ldeora")    return "LDEORA";
    if (low == "ldseta")    return "LDSETA";
    if (low == "casa")      return "CASA";
    if (low == "cas")       return "CAS";
    // Release-only atomics
    if (low == "ldaddl")    return "LDADDL";
    if (low == "ldclrl")    return "LDCLRL";
    if (low == "ldeorl")    return "LDEORL";
    if (low == "ldsetl")    return "LDSETL";
    if (low == "casl")      return "CASL";
    // Relaxed atomics / plain stores — no meaningful barrier in herd7 context
    return {};
}

// ---------------------------------------------------------------------------
// Map an Ordering value (from kExtFnSummary / kExtAnchors) to an appropriate
// synthetic Herd7 barrier that models the function's effect.
// ---------------------------------------------------------------------------
static std::string orderingToHerd7Barrier(Ordering ord)
{
    switch (ord) {
    case Ordering::SC:           return "DMB ISH";
    case Ordering::Acquire:      return "DMB LD";
    case Ordering::Release:      return "DMB ST";
    case Ordering::Relaxed:      // no hardware fence
    case Ordering::CompilerOnly: // compiler-only, no hardware fence
    case Ordering::None:
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// Collect ordering instructions from inline-asm within a single CallInst or
// CallBrInst that uses InlineAsm as its callee.
// ---------------------------------------------------------------------------
static void collectAsmOrderingInsts(const Instruction &I,
                                     std::vector<OrderingInst> &out)
{
    const InlineAsm *IA = nullptr;
    if (const auto *CI = dyn_cast<CallInst>(&I))
        IA = dyn_cast<InlineAsm>(CI->getCalledOperand());
    else if (const auto *CBR = dyn_cast<CallBrInst>(&I))
        IA = dyn_cast<InlineAsm>(CBR->getCalledOperand());
    if (!IA) return;

    std::string text = unescapeAsm(IA->getAsmString());
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip leading whitespace
        auto s0 = line.find_first_not_of(" \t");
        if (s0 == std::string::npos) continue;
        line = line.substr(s0);
        if (line.empty()) continue;

        // Skip directives, comments, labels
        char first = line[0];
        if (first == '.' || first == '/' || first == '#') continue;
        if (std::isdigit(static_cast<unsigned char>(first))) continue;
        {
            size_t colon = line.find(':');
            size_t space = line.find_first_of(" \t");
            if (colon != std::string::npos &&
                (space == std::string::npos || colon < space))
                continue;
        }

        // Tokenise up to two words
        auto nextTok = [&](const std::string &l, size_t &pos) -> std::string {
            size_t start = l.find_first_not_of(" \t", pos);
            if (start == std::string::npos) { pos = l.size(); return {}; }
            size_t end = l.find_first_of(" \t,", start);
            pos = (end == std::string::npos) ? l.size() : end;
            std::string tok = l.substr(start, pos - start);
            for (char &c : tok)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return tok;
        };

        size_t pos = 0;
        std::string w0 = nextTok(line, pos);
        std::string w1 = nextTok(line, pos);

        std::string mnemonic;
        if ((w0 == "dmb" || w0 == "dsb") && !w1.empty()) {
            if (!w1.empty() && w1.back() == ',') w1.pop_back();
            mnemonic = w0 + " " + w1;
        } else {
            mnemonic = w0;
        }

        std::string h7 = mnemonicToHerd7(mnemonic);
        if (h7.empty()) continue;

        // Determine ordering from kAsmAnchors
        Ordering ord = Ordering::None;
        for (const auto &a : kAsmAnchors) {
            if (a.mnemonic == mnemonic) {
                ord = a.ordering;
                break;
            }
        }
        if (ord <= Ordering::Relaxed) continue; // skip relaxed/plain

        // Mark whether this is a load or store instruction (needs register
        // operands in herd7 — cannot be emitted as a bare token).
        bool isLoad  = (w0 == "ldar"    || w0 == "ldarb"   || w0 == "ldarh"   ||
                        w0 == "ldaxr"   || w0 == "ldaxrb"  || w0 == "ldaxrh"  ||
                        w0 == "ldapr"   || w0 == "ldaprb"  || w0 == "ldaprh"  ||
                        w0 == "ldadda"  || w0 == "ldclra"  || w0 == "ldeora"  ||
                        w0 == "ldseta"  || w0 == "casa"    || w0 == "casal"   ||
                        w0 == "ldaddal" || w0 == "ldclral" || w0 == "ldeoral" ||
                        w0 == "ldsetal" || w0 == "swpal"   || w0 == "cas");
        bool isStore = (w0 == "stlr"   || w0 == "stlrb"  || w0 == "stlrh"  ||
                        w0 == "stlxr"  || w0 == "stlxrb" || w0 == "stlxrh" ||
                        w0 == "casl"   || w0 == "ldaddl" || w0 == "ldclrl" ||
                        w0 == "ldeorl" || w0 == "ldsetl");

        out.push_back({std::move(h7), ord, isLoad, isStore});
    }
}

// ---------------------------------------------------------------------------
// Collect ordering instructions from a call to a named external function.
// Checks kExtAnchors (prefix) then kExtFnSummary (exact).
// Also handles CallBrInst (used by some kernel asm macros).
// ---------------------------------------------------------------------------
static void collectExtFnOrderingInsts(const Instruction &I,
                                       std::vector<OrderingInst> &out)
{
    // Skip inline-asm calls — those are handled by collectAsmOrderingInsts.
    if (const auto *CI = dyn_cast<CallInst>(&I))
        if (CI->isInlineAsm()) return;

    Function *callee = nullptr;
    if (const auto *CI = dyn_cast<CallInst>(&I))
        callee = CI->getCalledFunction();
    else if (const auto *CBR = dyn_cast<CallBrInst>(&I))
        callee = CBR->getCalledFunction();
    if (!callee || !callee->isDeclaration() || callee->isIntrinsic()) return;

    std::string low = toLower(callee->getName());

    // 1. kExtAnchors — longest prefix match
    const ExtAnchor *bestEA = nullptr;
    size_t bestLen = 0;
    for (const auto &ea : kExtAnchors) {
        if (low.size() >= ea.prefix.size() &&
            low.compare(0, ea.prefix.size(), ea.prefix) == 0 &&
            ea.prefix.size() > bestLen) {
            bestEA  = &ea;
            bestLen = ea.prefix.size();
        }
    }
    if (bestEA && bestEA->ordering > Ordering::Relaxed) {
        std::string h7 = orderingToHerd7Barrier(bestEA->ordering);
        if (!h7.empty())
            out.push_back({std::move(h7), bestEA->ordering});
        return;
    }

    // 2. kExtFnSummary — exact name lookup
    const auto &tbl = extSummaryMap();
    auto it = tbl.find(std::string_view(low));
    if (it != tbl.end() && it->second->ordering > Ordering::Relaxed) {
        std::string h7 = orderingToHerd7Barrier(it->second->ordering);
        if (!h7.empty())
            out.push_back({std::move(h7), it->second->ordering});
    }
}

// ---------------------------------------------------------------------------
// Collect all ordering instructions between two instruction indices [lo, hi)
// (exclusive of the boundary instructions themselves) in a flattened
// instruction list.
//
// Only instructions that carry a hardware ordering above Relaxed are returned.
// Duplicate herd7 tokens (same string) are deduplicated, keeping the entry
// with the strongest ordering.
// ---------------------------------------------------------------------------
static std::vector<OrderingInst>
collectOrderingBetween(const std::vector<Instruction *> &insts,
                        int lo, int hi)
{
    // lo/hi are indices; we scan (lo, hi) exclusive.
    std::vector<OrderingInst> raw;
    for (int i = lo + 1; i < hi; ++i) {
        collectAsmOrderingInsts(*insts[i], raw);
        collectExtFnOrderingInsts(*insts[i], raw);
    }

    // Deduplicate by herd7 token, keeping the full OrderingInst from the
    // strongest-ordering occurrence (preserves isLoadInsn / isStoreInsn).
    std::map<std::string, OrderingInst> seen;
    for (auto &oi : raw) {
        auto it = seen.find(oi.herd7);
        if (it == seen.end() || oi.ordering > it->second.ordering)
            seen[oi.herd7] = oi;
    }

    // Reconstruct in original encounter order, first occurrence wins after
    // dedup.
    std::vector<OrderingInst> result;
    std::set<std::string> emitted;
    for (auto &oi : raw) {
        if (emitted.insert(oi.herd7).second)
            result.push_back(seen[oi.herd7]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Find the index of an anchor instruction in the flattened function body.
// Returns -1 if not found.
// ---------------------------------------------------------------------------
static int findAnchorInstIdx(const std::vector<Instruction *> &insts,
                              const AnchorHit &hit)
{
    for (int i = 0; i < (int)insts.size(); ++i) {
        const Instruction *I = insts[i];
        // Inline asm
        if (hit.site == "asm sideeffect") {
            const InlineAsm *IA = nullptr;
            if (const auto *CI = dyn_cast<CallInst>(I))
                IA = dyn_cast<InlineAsm>(CI->getCalledOperand());
            else if (const auto *CBR = dyn_cast<CallBrInst>(I))
                IA = dyn_cast<InlineAsm>(CBR->getCalledOperand());
            if (IA && IA->getAsmString().find(hit.label) != std::string::npos)
                return i;
        } else if (hit.site.size() > 1 && hit.site[0] == '@') {
            std::string callee = hit.site.substr(1);
            if (const auto *CI = dyn_cast<CallInst>(I))
                if (Function *fn = CI->getCalledFunction())
                    if (fn->getName() == callee)
                        return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Find the index of an instruction in `insts` whose printed IR text matches
// irInst.  Returns -1 if not found.
// ---------------------------------------------------------------------------
static int findInstByIR(const std::vector<Instruction *> &insts,
                         const std::string &irInst)
{
    for (int i = 0; i < (int)insts.size(); ++i) {
        std::string s;
        raw_string_ostream os(s);
        insts[i]->print(os);
        size_t start = s.find_first_not_of(" \t");
        if (start != std::string::npos) s = s.substr(start);
        size_t nl = s.find('\n');
        if (nl != std::string::npos) s = s.substr(0, nl);
        if (s == irInst)
            return i;
    }
    return -1;
}

// ===========================================================================
// Litmus-file generation
// ===========================================================================

// ---------------------------------------------------------------------------
// Shared-variable slot assignment.
// Each distinct shared object (identified by objDesc string) gets one litmus
// variable name x, y, z, w, v, u, t, s, r, q, ...
// ---------------------------------------------------------------------------
struct VarMap {
    std::map<std::string, std::string> objToVar;
    int counter = 0;

    const std::string &var(const std::string &obj) {
        auto it = objToVar.find(obj);
        if (it != objToVar.end())
            return it->second;
        // Generate name: x, y, z, a0, a1, ...
        std::string name;
        static const char *first[] = {"x","y","z","w","v","u","t","s","r","q"};
        if (counter < 10)
            name = first[counter];
        else
            name = "a" + std::to_string(counter - 10);
        ++counter;
        objToVar.emplace(obj, name);
        return objToVar.at(obj);
    }

    std::vector<std::string> allVars() const {
        std::vector<std::string> v;
        for (auto &[k, n] : objToVar)
            v.push_back(n);
        std::sort(v.begin(), v.end());
        return v;
    }
};

// ---------------------------------------------------------------------------
// RegisterFile — assigns per-thread virtual registers (X0, X1, …) to
// each shared-object variable access.
// ---------------------------------------------------------------------------
struct RegisterFile {
    std::map<std::string, std::string> varToReg; // varname → "X0"
    int counter = 0;
    int threadId = 0;

    explicit RegisterFile(int tid) : threadId(tid) {}

    // Allocate an address register for `var` if not already assigned.
    const std::string &addrReg(const std::string &var) {
        auto key = "addr:" + var;
        auto it = varToReg.find(key);
        if (it != varToReg.end())
            return it->second;
        std::string reg = "X" + std::to_string(counter++);
        varToReg.emplace(key, reg);
        return varToReg.at(key);
    }

    // Allocate a value register for `var` (for storing the loaded value).
    std::string valReg(const std::string &var) {
        auto key = "val:" + var;
        auto it = varToReg.find(key);
        if (it != varToReg.end())
            return it->second;
        std::string reg = "X" + std::to_string(counter++);
        varToReg.emplace(key, reg);
        return varToReg.at(key);
    }

    // Collect all (addrVar, addrReg) pairs for the init block.
    std::vector<std::pair<std::string,std::string>> addrInits() const {
        std::vector<std::pair<std::string,std::string>> res;
        for (auto &[k, r] : varToReg)
            if (k.size() > 5 && k.compare(0, 5, "addr:") == 0)
                res.emplace_back(k.substr(5), r);
        return res;
    }
};

// ---------------------------------------------------------------------------
// Convert an address register name (X-prefix) to its 32-bit W-prefix
// counterpart for data value instructions (STR/LDR/MOV).  Address registers
// always stay X-width; only value registers use W so memory sizes are
// consistent with the 32-bit `int` init declarations that herd7 requires.
static std::string toW(const std::string &reg)
{
    if (!reg.empty() && reg[0] == 'X')
        return "W" + reg.substr(1);
    return reg;
}

// Translate a SharedAccess to a sequence of Herd7 instructions and return
// a short description of the result register (for the exists clause).
//
// Returns the value register name (or "" for stores).
// Appends herd7 lines into `lines`.
// ---------------------------------------------------------------------------
static std::string emitAccess(const SharedAccess &sa,
                               VarMap &vars,
                               RegisterFile &regs,
                               std::vector<std::string> &lines)
{
    const std::string &var   = vars.var(sa.objDesc);
    const std::string &aReg  = regs.addrReg(var);

    if (sa.kind == SharedAccess::Kind::Load) {
        std::string vReg = regs.valReg(var);
        lines.push_back("LDR " + toW(vReg) + ",[" + aReg + "]");
        return vReg;
    } else {
        // Store: use W style for value — MOV + STR pattern
        std::string vReg = regs.valReg(var);
        lines.push_back("MOV " + toW(vReg) + ",#1");
        lines.push_back("STR " + toW(vReg) + ",[" + aReg + "]");
        return {};
    }
}

// ---------------------------------------------------------------------------
// Translate an anchor hit itself to one or more herd7 instructions.
// For asm anchors, derive the instruction from the label (mnemonic).
// For ext anchors, emit a DMB that models the call's ordering effect.
// ---------------------------------------------------------------------------
static void emitAnchor(const AnchorHit &hit,
                        const std::optional<SharedAccess> &anchorAlloc,
                        VarMap &vars,
                        RegisterFile &regs,
                        std::vector<std::string> &lines)
{
    // anchorAlloc: a shared object that the anchor writes to (channel variable).
    //
    // This path only applies to inline-ASM anchors whose mnemonic IS itself a
    // store-release (stlr, stlxr, swpal, casal, …).  In that case the asm
    // instruction writes to the anchorAlloc address.
    //
    // Lowering strategy depends on the anchor's ordering strength:
    //
    //   SC RMW (casal / swpal / ldaddal / ldclral / ldsetal / ldeoral):
    //     Emit the actual atomic RMW in herd7 syntax — preserves BOTH the
    //     acquire and release semantics so the AArch64 model can correctly
    //     reason about subsequent loads/stores in this thread.  Previously
    //     this path emitted a plain STLR (release only) which silently
    //     dropped the acquire side, producing "reversed-MP" false positives
    //     in the 48-case corpus (cases 13, 19–24).  We model the RMW as a
    //     SWPAL of an abstract new value — the actual operation kind
    //     (set/clear/add/eor) is immaterial to the ordering analysis.
    //
    //   Release-only (stlr, stlxr, casl, ldaddl, ldclrl, ldsetl, ldeorl):
    //     Plain STLR — already correct.
    //
    // External-function anchors (e.g. _printk, kfree, synchronize_rcu) are NOT
    // stores.  findAnchorAlloc may have found a pointer argument that happens to
    // alias a shared object, but the function does not actually do a
    // store-release to it.  Emitting STLR/SWPAL here would introduce a
    // spurious write and break the sync channel.  For external functions we
    // fall through to the barrier path.
    if (anchorAlloc && hit.site == "asm sideeffect") {
        const std::string &var  = vars.var(anchorAlloc->objDesc);
        const std::string &aReg = regs.addrReg(var);
        std::string vReg        = regs.valReg(var);

        // Detect SC-RMW mnemonic from the anchor label.
        std::string low = toLower(hit.label);
        bool isScRmw = (low == "casal"   || low == "swpal"   ||
                        low == "ldaddal" || low == "ldclral" ||
                        low == "ldsetal" || low == "ldeoral");
        if (isScRmw && hit.ordering == Ordering::SC) {
            // SWPAL Wnew, Wold, [Xaddr] — atomic acquire+release swap.
            // Wnew = #1, Wold = a scratch result reg we don't otherwise use.
            // Reusing vReg as the "new value" register is safe because we
            // never read the swap result anywhere else in P0.
            std::string oldReg = regs.valReg(var + ".swap_old");
            lines.push_back("MOV " + toW(vReg) + ",#1");
            lines.push_back("SWPAL " + toW(vReg) + "," +
                            toW(oldReg) + ",[" + aReg + "]");
            return;
        }

        // Release-only RMW or plain stlr/stlxr — STLR is sufficient.
        lines.push_back("MOV " + toW(vReg) + ",#1");
        lines.push_back("STLR " + toW(vReg) + ",[" + aReg + "]");
        return;
    }

    // No specific address — emit a barrier or the mnemonic directly.
    if (hit.site == "asm sideeffect") {
        std::string h7 = mnemonicToHerd7(toLower(hit.label));
        if (!h7.empty()) {
            // Pure barrier tokens (DMB/DSB) can be emitted as-is.
            // Load/store mnemonics (LDAR, STLR, CASA, STLXR, …) require
            // register operands in herd7 and must be lowered to the
            // equivalent pure barrier.
            static const char *const kPureBarriers[] = {
                "DMB ISH", "DMB ST", "DMB LD", "DMB OSH", "DSB SY"
            };
            bool isPure = false;
            for (auto b : kPureBarriers)
                if (h7 == b) { isPure = true; break; }
            if (isPure)
                lines.push_back(h7);
            else
                lines.push_back(orderingToHerd7Barrier(hit.ordering));
            return;
        }
    }

    // External function call — derive barrier from ordering.
    std::string h7 = orderingToHerd7Barrier(hit.ordering);
    if (!h7.empty())
        lines.push_back(h7);
}

// ---------------------------------------------------------------------------
// Emit ordering instructions as herd7 ops.
//
// Load/store mnemonics (LDAR, STLR, etc.) that require register operands
// are lowered to their pure-barrier equivalents (DMB LD, DMB ST, DMB ISH)
// so the output is always syntactically valid herd7.
// ---------------------------------------------------------------------------
static void emitOrderingInsts(const std::vector<OrderingInst> &ois,
                               std::vector<std::string> &lines)
{
    for (const auto &oi : ois) {
        if (oi.isLoadInsn || oi.isStoreInsn)
            lines.push_back(barrierEquiv(oi));
        else
            lines.push_back(oi.herd7);
    }
}

// ---------------------------------------------------------------------------
// Channel-aware variant used for the partner-thread prefix scan.
//
// When the anchor side has an anchorAlloc (i.e. the anchor is a release store
// to a channel variable), an LDAR in the partner thread is not just a barrier
// — it IS the acquire load of that channel.  Emit it as:
//
//   LDAR Xval,[Xaddr_of_channel]
//
// rather than as a bare "LDAR" token, so the litmus captures the actual
// channel read with its acquire semantics.  All other ordering instructions
// are emitted as bare barrier tokens as usual.
// ---------------------------------------------------------------------------
static void emitOrderingOrChannelLoad(
        const std::vector<OrderingInst> &ois,
        const std::optional<SharedAccess> &anchorAlloc,
        VarMap &vars,
        RegisterFile &regs,
        std::vector<std::string> &lines)
{
    for (const auto &oi : ois) {
        // Acquire-load instruction + known channel → explicit LDAR channel load
        if (oi.isLoadInsn && oi.ordering >= Ordering::Acquire && anchorAlloc) {
            const std::string &chanVar = vars.var(anchorAlloc->objDesc);
            const std::string &aReg   = regs.addrReg(chanVar);
            std::string vReg          = regs.valReg(chanVar);
            lines.push_back("LDAR " + toW(vReg) + ",[" + aReg + "]");
        } else if (oi.isLoadInsn || oi.isStoreInsn) {
            // Other load/store instructions: lower to barrier equivalent
            lines.push_back(barrierEquiv(oi));
        } else {
            lines.push_back(oi.herd7);
        }
    }
}

// ---------------------------------------------------------------------------
// Build the thread body for the anchor function (P0).
//
// Structure:
//   [before access]
//   [ordering insts between before and anchor]
//   [anchor instruction(s)]
//   [ordering insts between anchor and after]
//   [after access]
// ---------------------------------------------------------------------------
static std::vector<std::string>
buildAnchorThread(const AnchorBracket &br,
                  const std::vector<Instruction *> &insts,
                  VarMap &vars,
                  RegisterFile &regs)
{
    std::vector<std::string> lines;

    int beforeIdx = -1, anchorIdx = -1, afterIdx = -1;

    // Locate indices in flattened instruction list
    if (br.before)
        beforeIdx = findInstByIR(insts, br.before->irInst);
    anchorIdx = findAnchorInstIdx(insts, br.anchor);
    if (br.after)
        afterIdx = findInstByIR(insts, br.after->irInst);

    // --- before access ---
    if (br.before) {
        emitAccess(*br.before, vars, regs, lines);
    }

    // --- ordering between before and anchor ---
    if (beforeIdx >= 0 && anchorIdx >= 0 && beforeIdx < anchorIdx) {
        auto ois = collectOrderingBetween(insts, beforeIdx, anchorIdx);
        emitOrderingInsts(ois, lines);
    }

    // --- anchor itself ---
    emitAnchor(br.anchor, br.anchorAlloc, vars, regs, lines);

    // --- ordering between anchor and after ---
    if (anchorIdx >= 0 && afterIdx >= 0 && anchorIdx < afterIdx) {
        auto ois = collectOrderingBetween(insts, anchorIdx, afterIdx);
        emitOrderingInsts(ois, lines);
    }

    // --- after access ---
    if (br.after) {
        emitAccess(*br.after, vars, regs, lines);
    }

    return lines;
}

// ---------------------------------------------------------------------------
// Build the thread body for the partner function (P1).
//
// Scans the partner function body for ordering instructions:
//   • From the function start up to the first partner access (prefix).
//     This captures smp_load_acquire / LDAR which precedes heap accesses.
//   • Between every pair of consecutive partner accesses.
//
// The prefix scan uses emitOrderingOrChannelLoad so that an LDAR in this
// region is emitted as "LDAR Xval,[Xchan]" when a channel variable is known
// from anchorAlloc, rather than as a bare token.
// ---------------------------------------------------------------------------
static std::vector<std::string>
buildPartnerThread(const std::vector<PartnerAccess> &accesses,
                   const Function &partnerFn,
                   const AnchorBracket &br,
                   VarMap &vars,
                   RegisterFile &regs)
{
    std::vector<std::string> lines;
    if (accesses.empty())
        return lines;

    // Flatten partner function
    std::vector<Instruction *> insts;
    for (const BasicBlock &BB : partnerFn)
        for (const Instruction &I : BB)
            insts.push_back(const_cast<Instruction *>(&I));

    // Find indices of partner accesses in the flattened list
    std::vector<int> accessIdx;
    for (const auto &pa : accesses) {
        int idx = findInstByIR(insts, pa.irInst);
        accessIdx.push_back(idx);
    }

    // Channel variable: if the anchor is an inline-ASM store-release to a
    // channel object, a partner Load from that same object should be emitted
    // as LDAR (acquire load) rather than a plain LDR.
    //
    // For external-function anchors (e.g. _printk, kfree) emitAnchor now
    // emits a DMB barrier rather than an STLR, so the anchorAlloc variable is
    // never written by P0.  Using it as a "channel" would produce an LDAR of
    // a variable that is always 0 → degenerate test.
    bool anchorIsAsmStore = (br.anchorAlloc && br.anchor.site == "asm sideeffect");
    std::string chanVar;
    if (anchorIsAsmStore)
        chanVar = vars.var(br.anchorAlloc->objDesc);

    // Emit accesses in order, with ordering insts before/between them
    for (size_t i = 0; i < accesses.size(); ++i) {
        int hi = accessIdx[i];

        if (i == 0) {
            // Prefix scan: function start → first access.
            // lo = -1 so collectOrderingBetween starts from index 0.
            if (hi > 0) {
                auto ois = collectOrderingBetween(insts, /*lo=*/-1, hi);
                // Use channel-aware emitter only when the anchor is an
                // inline-ASM store-release: in that case the LDAR in P1's
                // prefix IS the channel acquire read and should be emitted
                // with its channel address rather than as a bare DMB LD.
                // For external-function anchors there is no channel STLR,
                // so the LDAR is just a load barrier (emit as DMB LD).
                const std::optional<SharedAccess> *chanHint =
                    anchorIsAsmStore ? &br.anchorAlloc : nullptr;
                emitOrderingOrChannelLoad(ois,
                    chanHint ? *chanHint : std::optional<SharedAccess>{},
                    vars, regs, lines);
            }
        } else {
            // Between previous access and this one
            int lo = accessIdx[i - 1];
            if (lo >= 0 && hi >= 0 && lo < hi) {
                auto ois = collectOrderingBetween(insts, lo, hi);
                emitOrderingInsts(ois, lines);
            }
        }

        // Build a SharedAccess from PartnerAccess for emitAccess
        SharedAccess sa;
        sa.objDesc = accesses[i].objDesc;
        sa.irInst  = accesses[i].irInst;
        // Determine kind from IR text
        const std::string &ir = accesses[i].irInst;
        bool isStore = (ir.size() >= 6 && ir.compare(0, 6, "store ") == 0)
                    || ir.find("stlr")  != std::string::npos
                    || ir.find("stlxr") != std::string::npos
                    || ir.find("swpal") != std::string::npos;
        sa.kind = isStore ? SharedAccess::Kind::Store : SharedAccess::Kind::Load;

        // If this is a Load from the channel object, emit as LDAR (acquire
        // load) rather than a plain LDR.  This covers the common kernel
        // pattern where smp_load_acquire(&chan) lowers to an inline-asm
        // "ldar" CallInst whose argument is the channel pointer.  That
        // CallInst is recorded by FunctionPairsPass as a partner access for
        // the channel object, but emitAccess would only emit a plain LDR.
        bool isChanLoad = sa.kind == SharedAccess::Kind::Load
                       && !chanVar.empty()
                       && vars.var(sa.objDesc) == chanVar;
        if (isChanLoad) {
            const std::string &aReg = regs.addrReg(chanVar);
            std::string vReg        = regs.valReg(chanVar);
            lines.push_back("LDAR " + toW(vReg) + ",[" + aReg + "]");
        } else {
            emitAccess(sa, vars, regs, lines);
        }
    }

    return lines;
}

// ---------------------------------------------------------------------------
// Format column-aligned thread bodies for the litmus file.
// Each vector entry is one instruction line.  Threads are separated by " | ".
// Returns the body section as a string.
// ---------------------------------------------------------------------------
static std::string formatThreadColumns(
    const std::string &p0name,
    const std::vector<std::string> &p0,
    const std::string &p1name,
    const std::vector<std::string> &p1)
{
    size_t rows = std::max(p0.size(), p1.size());

    // Determine column width
    size_t w0 = p0name.size();
    for (const auto &l : p0) w0 = std::max(w0, l.size());
    w0 += 2; // padding

    std::ostringstream os;
    // Header
    os << " " << p0name;
    os << std::string(w0 - p0name.size(), ' ') << "| " << p1name << " ;\n";

    for (size_t r = 0; r < rows; ++r) {
        const std::string &l0 = r < p0.size() ? p0[r] : "";
        const std::string &l1 = r < p1.size() ? p1[r] : "";
        os << " " << l0;
        os << std::string(w0 - l0.size(), ' ') << "| " << l1 << " ;\n";
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// Build the "exists" / "~exists" clause.
//
// Returns an empty string for degenerate tests (where no condition can
// meaningfully hold or fail based on memory ordering); callers must skip
// generating the litmus file in that case.
//
// Herd7 semantics:
//   ~exists (cond)  — the condition is FORBIDDEN; herd7 reports "Never" if
//                     the ordering prevents it.  Use for safety properties
//                     where the race outcome must be impossible.
//   exists (cond)   — the condition is OBSERVABLE; herd7 reports "Sometimes"
//                     or "Always" if the outcome is reachable.  Use when P1
//                     only has a single load (nothing to synchronise with).
//
// Parameters:
//   p0WrittenVarNames — variable names (x, y, z, …) that P0 writes to;
//                       derived from br.before (Store), br.anchorAlloc,
//                       and br.after (Store).
//   p1StoreVarNames   — variable names that P1 stores to; derived from
//                       pair.partnerAccesses with isStore == true.
//
// Decision logic:
//
// (A) P1 has ≥ 2 LOAD value registers — there is a synchronisation register
//     and at least one payload register.  This covers MP patterns:
//       • MP release/acquire   (STLR/LDAR)
//       • MP via DMB           (STR + DMB + STR / LDR + LDR)
//       • Lock-based handoff, RCU, seqlock, CAS-based stacks, etc.
//
//     The "channel" (sync) register is:
//       • The LOAD val-reg for the anchorAlloc object if P1 loaded it.
//       • Otherwise the first LOAD val-reg for any P0-written variable.
//       • If none exist, the test is degenerate → return "".
//
//     Payload conditions only include LOAD val-regs for variables that P0
//     writes (so the condition can non-trivially be 0 or 1).  Variables that
//     nobody writes stay at their initial value 0 forever, making payload=0
//     always true regardless of ordering — producing false "No" verdicts.
//
//     If all payload vars are degenerate (unwritten), downgrade to Case (B).
//
// (B) P1 has exactly 1 LOAD value register:
//       • If P0 writes that variable → "exists (1:reg=1)".
//       • Otherwise nobody ever writes it, the condition can never hold,
//         and the test is degenerate → return "".
//
// (C) P1 has no LOAD value registers (all stores, or no accesses) →
//     return "" (degenerate).
//
// NOTE: Store value registers in P1 hold a compile-time constant (#1)
// and do NOT reflect what P1 observed from shared memory.  They must not
// appear in the exists clause; using them would make sub-conditions
// trivially always true, causing false "No" verdicts.
// ---------------------------------------------------------------------------
static std::string buildExistsClause(
        const AnchorBracket &br,
        VarMap &vars,
        const RegisterFile &r1,
        const std::set<std::string> &p0WrittenVarNames,
        const std::set<std::string> &p1StoreVarNames)
{
    // Collect P1's LOAD value registers only.
    // A val reg is a "load" reg when its variable is NOT in p1StoreVarNames.
    // This also captures channel val-regs created by emitOrderingOrChannelLoad
    // (LDAR in the prefix scan) which are not listed in pair.partnerAccesses.
    std::vector<std::pair<std::string, std::string>> loadValRegs; // (key, reg)
    for (const auto &[k, reg] : r1.varToReg) {
        if (k.compare(0, 4, "val:") != 0) continue;
        std::string varName = k.substr(4);
        if (!p1StoreVarNames.count(varName))
            loadValRegs.push_back({k, reg});
    }

    // (C) No load registers → degenerate
    if (loadValRegs.empty())
        return "";

    if (loadValRegs.size() == 1) {
        // (B) Single LOAD in P1: only meaningful if P0 writes this variable.
        std::string varName = loadValRegs[0].first.substr(4);
        if (p0WrittenVarNames.count(varName))
            return "exists (1:" + loadValRegs[0].second + "=1)";
        // P0 never writes this var → its value is always the initial 0,
        // so the condition can never hold → degenerate.
        return "";
    }

    // (A) ≥ 2 LOAD value registers: try to form ~exists (sync=1 /\ payload=0).
    //
    // Choose the synchronisation (channel) register:
    //   • Prefer the LOAD val-reg for the anchorAlloc variable if P1 loaded it,
    //     BUT only when the anchor is an inline-ASM store (the ASM itself IS the
    //     STLR to that address).  For external-function anchors emitAnchor emits
    //     a DMB barrier rather than an STLR, so the anchorAlloc variable is never
    //     written by P0 in the litmus — using it as the sync variable would make
    //     the sync condition trivially false (initial value 0 forever).
    //   • Fall back to the first LOAD val-reg for any P0-written variable.
    std::string syncReg, syncKey;
    if (br.anchorAlloc &&
        br.anchor.site == "asm sideeffect" &&
        (br.anchor.ordering == Ordering::Release ||
         br.anchor.ordering == Ordering::SC))
    {
        const std::string chanVar = vars.var(br.anchorAlloc->objDesc);
        // Only use as sync if this is genuinely a LOAD (not a store) reg.
        if (!p1StoreVarNames.count(chanVar)) {
            const std::string chanKey = "val:" + chanVar;
            auto it = r1.varToReg.find(chanKey);
            if (it != r1.varToReg.end()) {
                syncReg = it->second;
                syncKey = chanKey;
            }
        }
    }
    if (syncReg.empty()) {
        // Fallback: first LOAD val-reg for a P0-written variable.
        for (const auto &[k, reg] : loadValRegs) {
            std::string vn = k.substr(4);
            if (p0WrittenVarNames.count(vn)) {
                syncReg = reg;
                syncKey = k;
                break;
            }
        }
    }
    if (syncReg.empty())
        return ""; // no P0-written var is loaded by P1 → degenerate

    // Payload: other LOAD val-regs for P0-written variables.
    // Exclude vars never written by P0: reading an unwritten var always yields
    // its initial value (0), making "payload=0" trivially true regardless of
    // memory ordering — a source of false "No" verdicts.
    std::vector<std::string> payloadConds;
    for (const auto &[k, reg] : loadValRegs) {
        if (k == syncKey) continue;
        std::string vn = k.substr(4);
        if (p0WrittenVarNames.count(vn))
            payloadConds.push_back("1:" + reg + "=0");
    }

    if (payloadConds.empty()) {
        // Only one useful variable: downgrade to a single-var exists clause.
        return "exists (1:" + syncReg + "=1)";
    }

    // Full ~exists forbidden-outcome clause.
    std::ostringstream os;
    os << "(* forbidden: P1 observes synchronisation object written "
          "but reads stale payload *)\n";
    os << "~exists (1:" << syncReg << "=1";
    for (const auto &pc : payloadConds)
        os << " /\\ " << pc;
    os << ")";
    return os.str();
}

// ---------------------------------------------------------------------------
// Sanitise a string for use in a litmus test name (no spaces or special chars)
// ---------------------------------------------------------------------------
static std::string sanitise(const std::string &s)
{
    std::string r;
    for (char c : s)
        r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return r;
}

} // anonymous namespace

// ===========================================================================
// Pass implementation
// ===========================================================================

Herd7TranspilerResult
Herd7TranspilerPass::run(Module &M, ModuleAnalysisManager &MAM)
{
    mbtime::Scope _t("transpiler");

    Herd7TranspilerResult result;

    // ── Obtain filtered function pairs ────────────────────────────────────────
    //
    // Only generate litmus tests for pairs that are genuinely concurrent —
    // i.e. pairs that survived all mutual-exclusion heuristics in
    // FnHeuristicsFilterPass (call graph, lock alias, lifecycle, RCU, etc.).
    // Pairs suppressed by the filter are statically serialised and do not
    // need a litmus test.
    const FnHeuristicsFilterResult &filteredPairs =
        MAM.getResult<FnHeuristicsFilterPass>(M);

    if (filteredPairs.byAnchorFn.empty())
        return result;

    // ── Process each pair ────────────────────────────────────────────────────
    // Track how many litmus files we've emitted per anchor-function pair to
    // generate unique filenames when there are multiple brackets.
    std::map<std::string, int> pairCounter;

    for (const auto &[anchorFnName, pairs] : filteredPairs.byAnchorFn) {
        Function *anchorFn = M.getFunction(anchorFnName);
        if (!anchorFn || anchorFn->isDeclaration())
            continue;

        // Flatten anchor function once
        std::vector<Instruction *> anchorInsts;
        for (BasicBlock &BB : *anchorFn)
            for (Instruction &I : BB)
                anchorInsts.push_back(&I);

        for (const FunctionPair &pair : pairs) {
            const AnchorBracket &br = pair.bracket;

            Function *partnerFn = M.getFunction(pair.partnerFn);
            if (!partnerFn || partnerFn->isDeclaration())
                continue;

            // Skip pairs with no shared accesses to generate from
            if (!br.before && !br.after && pair.partnerAccesses.empty())
                continue;

            // ── Build per-pair variable and register maps ──────────────────
            VarMap    vars;
            RegisterFile r0(0), r1(1);

            // Pre-register all shared objects so addr regs are consistent
            auto regObj = [&](const std::string &obj) {
                vars.var(obj);          // ensure slot exists
                r0.addrReg(vars.var(obj));
                r1.addrReg(vars.var(obj));
            };

            if (br.before)      regObj(br.before->objDesc);
            if (br.anchorAlloc) regObj(br.anchorAlloc->objDesc);
            if (br.after)       regObj(br.after->objDesc);
            for (const auto &pa : pair.partnerAccesses)
                regObj(pa.objDesc);

            // ── Build thread bodies ─────────────────────────────────────────
            std::vector<std::string> p0lines =
                buildAnchorThread(br, anchorInsts, vars, r0);

            std::vector<std::string> p1lines =
                buildPartnerThread(pair.partnerAccesses, *partnerFn, br, vars, r1);

            if (p0lines.empty() && p1lines.empty())
                continue;

            // ── Assemble init block ─────────────────────────────────────────
            // All shared variables start at 0.
            // Address registers for P0 and P1 are initialised to point to them.
            std::ostringstream init;
            init << "{\n";
            for (const auto &vname : vars.allVars())
                init << "int " << vname << "=0;\n";
            // P0 address registers
            for (const auto &[vname, reg] : r0.addrInits())
                init << "0:" << reg << "=" << vname << ";\n";
            // P1 address registers
            for (const auto &[vname, reg] : r1.addrInits())
                init << "1:" << reg << "=" << vname << ";\n";
            init << "}";

            // ── Build exists clause ─────────────────────────────────────────
            // p0WrittenVarNames: variables that P0 stores to (before-store,
            // anchorAlloc, after-store).  Used to filter degenerate payload
            // conditions where the variable is never written and therefore
            // always holds its initial value 0.
            //
            // anchorAlloc is counted as a write ONLY for inline-ASM anchors
            // whose mnemonic is a store-release (stlr, swpal, …).  For
            // external-function anchors (e.g. _printk, kfree) emitAnchor now
            // emits a DMB barrier — not an STLR — so the anchorAlloc variable
            // is NOT written in the litmus thread and must not appear here.
            std::set<std::string> p0WrittenVarNames;
            if (br.before && br.before->kind == SharedAccess::Kind::Store)
                p0WrittenVarNames.insert(vars.var(br.before->objDesc));
            if (br.anchorAlloc && br.anchor.site == "asm sideeffect")
                p0WrittenVarNames.insert(vars.var(br.anchorAlloc->objDesc));
            if (br.after && br.after->kind == SharedAccess::Kind::Store)
                p0WrittenVarNames.insert(vars.var(br.after->objDesc));

            // p1StoreVarNames: variables that P1 stores to.  Store val-regs
            // hold a compile-time constant and must not appear in the exists
            // clause as if they were observed loads.
            std::set<std::string> p1StoreVarNames;
            for (const auto &pa : pair.partnerAccesses) {
                const std::string &paIR = pa.irInst;
                bool paIsStore =
                    (paIR.size() >= 6 && paIR.compare(0, 6, "store ") == 0)
                    || paIR.find("stlr")  != std::string::npos
                    || paIR.find("stlxr") != std::string::npos
                    || paIR.find("swpal") != std::string::npos;
                if (paIsStore)
                    p1StoreVarNames.insert(vars.var(pa.objDesc));
            }

            std::string exists =
                buildExistsClause(br, vars, r1, p0WrittenVarNames,
                                  p1StoreVarNames);
            // Skip degenerate tests: conditions that can never meaningfully
            // hold or fail due to missing writes to the tested variables.
            if (exists.empty())
                continue;

            // ── Assemble litmus text ────────────────────────────────────────
            std::string pairKey = sanitise(anchorFnName) + "_" +
                                  sanitise(pair.partnerFn);
            int idx = pairCounter[pairKey]++;
            std::string litmusName = pairKey +
                                     (idx ? "_" + std::to_string(idx) : "");
            std::string filename   = litmusName + ".litmus";

            std::ostringstream lit;
            lit << "AArch64 " << litmusName << "\n";
            lit << "(* anchor: " << br.anchor.label
                << "  [" << orderingStr(br.anchor.ordering) << "]"
                << "  (" << br.anchor.site << ") *)\n";
            lit << "(* anchor-fn: " << anchorFnName
                << "  partner-fn: " << pair.partnerFn << " *)\n";
            lit << init.str() << "\n\n";
            lit << formatThreadColumns("P0", p0lines, "P1", p1lines);
            lit << exists << "\n";

            result.litmusFiles.emplace(std::move(filename), lit.str());
        }
    }

    return result;
}
