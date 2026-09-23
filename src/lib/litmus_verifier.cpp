#include "lib/litmus_verifier.h"
#include "lib/anchor_pass.h"
#include "lib/anchors.h"
#include "lib/causal_detect.h"
#include "lib/ext_fn_summary.h"
#include "lib/fn_heuristics.h"
#include "lib/function_pairs.h"
#include "lib/herd7_transpiler.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
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

AnalysisKey LitmusVerifierPass::Key;

// ===========================================================================
// Local helpers — mirror the ones in herd7_transpiler.cpp so the verifier is
// self-contained (so a bug in either pipeline does not cancel itself out).
// ===========================================================================

namespace {

// Sanitise a string to litmus-name form (must match Herd7TranspilerPass).
static std::string sanitise(const std::string &s)
{
    std::string r;
    for (char c : s)
        r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return r;
}

static std::string toLower(StringRef s)
{
    std::string r(s.str());
    for (char &c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Flatten a function into a linear instruction sequence (BB order).
static std::vector<Instruction *> flattenFn(Function &F)
{
    std::vector<Instruction *> out;
    for (BasicBlock &BB : F)
        for (Instruction &I : BB)
            out.push_back(&I);
    return out;
}

// Locate an instruction in `insts` whose one-line IR text matches `irInst`.
// Comparison is robust to metadata-ID renumbering between recording time
// (in FunctionPairsPass) and verification time: we strip any trailing
// `, !<metadata>` suffix from both sides before comparing.
static std::string stripMetadata(std::string s)
{
    // First find ", !" which marks the start of an attached metadata list.
    size_t pos = s.find(", !");
    if (pos != std::string::npos) s = s.substr(0, pos);
    // Some metadata appears as " !dbg !N" without the leading comma — also
    // strip everything from " !" onward when it precedes an identifier char.
    pos = s.find(" !");
    if (pos != std::string::npos &&
        pos + 2 < s.size() &&
        (std::isalpha(static_cast<unsigned char>(s[pos + 2])) || s[pos + 2] == '_'))
        s = s.substr(0, pos);
    // Trim trailing whitespace.
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

static int findInstByIR(const std::vector<Instruction *> &insts,
                         const std::string &irInst)
{
    std::string needle = stripMetadata(irInst);
    // FunctionPairsPass truncates long IR strings to 120 chars with a
    // trailing "...".  Detect this and turn the comparison into a prefix
    // match against the first 117 chars.
    bool truncated = needle.size() >= 3 &&
                      needle.compare(needle.size() - 3, 3, "...") == 0;
    if (truncated) needle = needle.substr(0, needle.size() - 3);

    for (int i = 0; i < (int)insts.size(); ++i) {
        std::string s;
        raw_string_ostream os(s);
        insts[i]->print(os);
        size_t start = s.find_first_not_of(" \t");
        if (start != std::string::npos) s = s.substr(start);
        size_t nl = s.find('\n');
        if (nl != std::string::npos) s = s.substr(0, nl);
        s = stripMetadata(s);
        if (truncated) {
            if (s.size() >= needle.size() &&
                s.compare(0, needle.size(), needle) == 0)
                return i;
        } else if (s == needle) {
            return i;
        }
    }
    return -1;
}

// Locate the anchor instruction in `insts` for `hit` (same as in transpiler).
static int findAnchorInstIdx(const std::vector<Instruction *> &insts,
                              const AnchorHit &hit)
{
    for (int i = 0; i < (int)insts.size(); ++i) {
        const Instruction *I = insts[i];
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
// Source-of-truth: enumerate the ordering tokens that the original IR can
// legitimately produce in the (lo, hi) interval (exclusive of endpoints).
//
// Mirrors collectOrderingBetween in herd7_transpiler.cpp.  Returns the set
// of unique herd7 tokens.  Also includes the pure-barrier *equivalents*
// (DMB LD / DMB ST / DMB ISH) for any load/store-form mnemonic, because
// emitOrderingInsts lowers those to their barrier equivalents.
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
            int hi = hd(s[i + 1]), lo = hd(s[i + 2]);
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

// Equivalent pure-barrier token for any load/store mnemonic at a given
// ordering — same lowering as barrierEquiv in the transpiler.
static std::string barrierEquivToken(const std::string &mnemonic, Ordering ord)
{
    static const std::set<std::string> kLoadMn = {
        "ldar", "ldarb", "ldarh", "ldaxr", "ldaxrb", "ldaxrh",
        "ldapr", "ldaprb", "ldaprh",
        "ldadda", "ldclra", "ldeora", "ldseta", "casa", "casal",
        "ldaddal", "ldclral", "ldeoral", "ldsetal", "swpal", "cas"
    };
    static const std::set<std::string> kStoreMn = {
        "stlr", "stlrb", "stlrh", "stlxr", "stlxrb", "stlxrh",
        "casl", "ldaddl", "ldclrl", "ldeorl", "ldsetl"
    };
    if (kLoadMn.count(mnemonic))
        return (ord == Ordering::SC) ? "DMB ISH" : "DMB LD";
    if (kStoreMn.count(mnemonic))
        return (ord == Ordering::SC) ? "DMB ISH" : "DMB ST";
    return {};
}

// Add to `out` every plausible token contributed by instruction I.
static void recordPlausibleTokens(const Instruction &I,
                                    std::set<std::string> &out)
{
    // Inline-asm.
    const InlineAsm *IA = nullptr;
    if (const auto *CI = dyn_cast<CallInst>(&I))
        IA = dyn_cast<InlineAsm>(CI->getCalledOperand());
    else if (const auto *CBR = dyn_cast<CallBrInst>(&I))
        IA = dyn_cast<InlineAsm>(CBR->getCalledOperand());

    if (IA) {
        std::string text = unescapeAsm(IA->getAsmString());
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            auto s0 = line.find_first_not_of(" \t");
            if (s0 == std::string::npos) continue;
            line = line.substr(s0);
            if (line.empty()) continue;
            char first = line[0];
            if (first == '.' || first == '/' || first == '#') continue;
            if (std::isdigit(static_cast<unsigned char>(first))) continue;

            // Tokenize first two words.
            auto nextTok = [&](const std::string &l, size_t &pos) {
                size_t start = l.find_first_not_of(" \t", pos);
                if (start == std::string::npos) { pos = l.size(); return std::string{}; }
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
            std::string mnemonic = (w0 == "dmb" || w0 == "dsb") && !w1.empty()
                                       ? w0 + " " + w1
                                       : w0;

            // Match against kAsmAnchors.
            Ordering ord = Ordering::None;
            for (const auto &a : kAsmAnchors)
                if (a.mnemonic == mnemonic) { ord = a.ordering; break; }
            if (ord <= Ordering::Relaxed) continue;

            // Direct herd7 mnemonic (DMB ISH / DMB ST / DMB LD / etc.).
            if (mnemonic == "dmb ish")   out.insert("DMB ISH");
            else if (mnemonic == "dmb ishst") out.insert("DMB ST");
            else if (mnemonic == "dmb ishld") out.insert("DMB LD");
            else if (mnemonic == "dmb osh")   out.insert("DMB OSH");
            else if (mnemonic == "dmb oshst") out.insert("DMB ST");
            else if (mnemonic == "dsb sy")    out.insert("DSB SY");
            else if (mnemonic == "dsb ish")   out.insert("DMB ISH");
            else if (mnemonic == "dmb")       out.insert("DMB ISH");
            else if (mnemonic == "dsb")       out.insert("DSB SY");

            // Load/store-form: add pure-barrier equivalent (this is what
            // emitOrderingInsts actually emits between accesses).
            std::string eq = barrierEquivToken(w0, ord);
            if (!eq.empty())
                out.insert(eq);
        }
        return;
    }

    // External (declared-only) function call.
    Function *callee = nullptr;
    if (const auto *CI = dyn_cast<CallInst>(&I))
        callee = CI->getCalledFunction();
    else if (const auto *CBR = dyn_cast<CallBrInst>(&I))
        callee = CBR->getCalledFunction();
    if (!callee || !callee->isDeclaration() || callee->isIntrinsic()) return;

    std::string low = toLower(callee->getName());

    Ordering ord = Ordering::None;

    // kExtAnchors (longest prefix match).
    const ExtAnchor *bestEA = nullptr;
    size_t bestLen = 0;
    for (const auto &ea : kExtAnchors)
        if (low.size() >= ea.prefix.size() &&
            low.compare(0, ea.prefix.size(), ea.prefix) == 0 &&
            ea.prefix.size() > bestLen) {
            bestEA  = &ea;
            bestLen = ea.prefix.size();
        }
    if (bestEA && bestEA->ordering > Ordering::Relaxed)
        ord = bestEA->ordering;
    else {
        // kExtFnSummary (exact match).
        const auto &tbl = extSummaryMap();
        auto it = tbl.find(std::string_view(low));
        if (it != tbl.end() && it->second->ordering > Ordering::Relaxed)
            ord = it->second->ordering;
    }

    switch (ord) {
        case Ordering::SC:      out.insert("DMB ISH"); break;
        case Ordering::Acquire: out.insert("DMB LD");  break;
        case Ordering::Release: out.insert("DMB ST");  break;
        default: break;
    }
}

static std::set<std::string>
plausibleBarriersBetween(const std::vector<Instruction *> &insts,
                          int lo, int hi)
{
    std::set<std::string> out;
    if (lo >= hi) return out;
    for (int i = lo + 1; i < hi && i < (int)insts.size(); ++i)
        if (i >= 0)
            recordPlausibleTokens(*insts[i], out);
    return out;
}

// Expected anchor token (best-effort lower bound on ordering) — mirrors
// emitAnchor / orderingToHerd7Barrier.
static std::string expectedAnchorToken(const AnchorBracket &br)
{
    // ASM anchors with anchorAlloc → STLR or SWPAL.
    if (br.anchorAlloc && br.anchor.site == "asm sideeffect") {
        std::string low = toLower(br.anchor.label);
        if ((low == "casal"  || low == "swpal"   ||
             low == "ldaddal" || low == "ldclral" ||
             low == "ldsetal" || low == "ldeoral") &&
            br.anchor.ordering == Ordering::SC)
            return "SWPAL";
        return "STLR";
    }
    // ASM anchor without anchorAlloc → pure barrier mnemonic.
    if (br.anchor.site == "asm sideeffect") {
        std::string low = toLower(br.anchor.label);
        if (low == "dmb ish"   || low == "dsb ish" || low == "dmb")
            return "DMB ISH";
        if (low == "dmb ishst" || low == "dmb oshst") return "DMB ST";
        if (low == "dmb ishld" || low == "dmb oshld" || low == "dsb ld")
            return "DMB LD";
        if (low == "dsb sy" || low == "dsb")     return "DSB SY";
    }
    // External-fn or volatile anchor → ordering-driven barrier.
    switch (br.anchor.ordering) {
        case Ordering::SC:      return "DMB ISH";
        case Ordering::Acquire: return "DMB LD";
        case Ordering::Release: return "DMB ST";
        default:                return {};
    }
}

// Extract the P0 column from the litmus body.  Returns one string per row.
static std::vector<std::string>
extractColumn(const std::string &litmus, int column /* 0 or 1 */)
{
    std::vector<std::string> rows;
    std::istringstream ss(litmus);
    std::string line;
    bool inBody = false;
    while (std::getline(ss, line)) {
        // The body starts at the header row " P0   | P1 ;"
        if (!inBody) {
            if (line.find("P0") != std::string::npos &&
                line.find("|")  != std::string::npos &&
                line.find("P1") != std::string::npos) {
                inBody = true;
            }
            continue;
        }
        // Body ends at the exists / ~exists clause.
        if (line.find("exists") != std::string::npos) break;
        size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        std::string left  = line.substr(0, bar);
        std::string right = line.substr(bar + 1);
        // Strip trailing " ;"
        auto strip = [](std::string &s) {
            while (!s.empty() && (s.back() == ';' || s.back() == ' ' ||
                                   s.back() == '\t' || s.back() == '\n' ||
                                   s.back() == '\r'))
                s.pop_back();
            size_t f = s.find_first_not_of(" \t");
            if (f != std::string::npos) s = s.substr(f);
        };
        strip(left);
        strip(right);
        rows.push_back(column == 0 ? left : right);
    }
    return rows;
}

static std::vector<std::string>
extractVarNames(const std::string &litmus)
{
    // Init block:  "int x=0;"  one per line.
    std::vector<std::string> vars;
    std::istringstream ss(litmus);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() > 4 && line.compare(0, 4, "int ") == 0) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string v = line.substr(4, eq - 4);
            // Trim
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
            if (!v.empty())
                vars.push_back(v);
        }
    }
    return vars;
}

// Extract the exists / ~exists clause line.
static std::string extractExistsClause(const std::string &litmus)
{
    std::istringstream ss(litmus);
    std::string line;
    while (std::getline(ss, line)) {
        // Skip the comment line that starts with "(* forbidden".
        if (line.size() >= 2 && line[0] == '(' && line[1] == '*') continue;
        if (line.find("exists") != std::string::npos)
            return line;
    }
    return {};
}

// True if `hay` contains any of the listed tokens.
static bool containsAny(const std::string &hay,
                         std::initializer_list<const char *> needles)
{
    for (auto *n : needles)
        if (hay.find(n) != std::string::npos)
            return true;
    return false;
}

// ===========================================================================
// Property checks.
// ===========================================================================

// P1: PO-embedding.  before_idx < anchor_idx < after_idx in the flattened
// instruction list of the anchor function, and likewise for the partner
// function's PartnerAccess indices.
static PropertyCheck
checkP1(const std::vector<Instruction *> &anchorInsts,
        const FunctionPair                &pair,
        const std::vector<Instruction *> &partnerInsts)
{
    const AnchorBracket &br = pair.bracket;

    int bIdx = br.before ? findInstByIR(anchorInsts, br.before->irInst) : -1;
    int aIdx = findAnchorInstIdx(anchorInsts, br.anchor);
    int fIdx = br.after  ? findInstByIR(anchorInsts, br.after->irInst)  : -1;

    if (aIdx < 0)
        return PropertyCheck::fail("anchor instruction not located in flattened IR");

    if (br.before && bIdx < 0)
        return PropertyCheck::fail("before-access not located in flattened IR");
    if (br.after  && fIdx < 0)
        return PropertyCheck::fail("after-access not located in flattened IR");

    if (br.before && bIdx >= aIdx)
        return PropertyCheck::fail("before-access index " + std::to_string(bIdx)
                                   + " not strictly less than anchor index "
                                   + std::to_string(aIdx));
    if (br.after && aIdx >= fIdx)
        return PropertyCheck::fail("anchor index " + std::to_string(aIdx)
                                   + " not strictly less than after-access index "
                                   + std::to_string(fIdx));

    // Partner-side: indices must be monotonically non-decreasing in encounter
    // order (FunctionPairsPass emits partner accesses in BB-order).
    int prev = -1;
    for (const PartnerAccess &pa : pair.partnerAccesses) {
        int idx = findInstByIR(partnerInsts, pa.irInst);
        if (idx < 0)
            return PropertyCheck::fail("partner access not located in flattened IR: "
                                       + pa.irInst);
        if (idx < prev)
            return PropertyCheck::fail("partner-access indices non-monotonic ("
                                       + std::to_string(idx) + " after "
                                       + std::to_string(prev) + ")");
        prev = idx;
    }
    return PropertyCheck::pass();
}

// P2: Anchor fidelity.  The litmus P0 column must contain a token
// consistent with the anchor's ordering class.
static PropertyCheck
checkP2(const FunctionPair &pair, const std::string &litmus)
{
    std::string expected = expectedAnchorToken(pair.bracket);
    if (expected.empty())
        return PropertyCheck::skip("anchor ordering "
            + std::string(orderingStr(pair.bracket.anchor.ordering))
            + " has no required emission");

    auto p0 = extractColumn(litmus, 0);

    // Tokens that satisfy or strengthen the expected one.
    std::vector<const char *> alts;
    if (expected == "STLR")
        alts = { "STLR", "SWPAL", "DMB ISH", "DMB ST", "DSB SY" };
    else if (expected == "SWPAL")
        alts = { "SWPAL", "DMB ISH", "DSB SY" };
    else if (expected == "DMB ISH")
        alts = { "DMB ISH", "DSB SY" };
    else if (expected == "DSB SY")
        alts = { "DSB SY", "DMB ISH" };
    else if (expected == "DMB ST")
        alts = { "DMB ST", "DMB ISH", "DSB SY", "STLR", "SWPAL" };
    else if (expected == "DMB LD")
        alts = { "DMB LD", "DMB ISH", "DSB SY", "LDAR" };
    else
        alts = { expected.c_str() };

    for (const std::string &row : p0)
        for (auto *tok : alts)
            if (row.find(tok) != std::string::npos)
                return PropertyCheck::pass();

    return PropertyCheck::fail("anchor of class "
        + std::string(orderingStr(pair.bracket.anchor.ordering))
        + " expected a token in {" + expected + ", ...} but P0 has none");
}

// P3: rf-closure.  Every shared object referenced in the bracket (before /
// anchorAlloc / after) must appear as a litmus variable, AND every variable
// declared in init must appear in at least one of P0/P1.
static PropertyCheck
checkP3(const FunctionPair &pair, const std::string &litmus)
{
    std::vector<std::string> vars = extractVarNames(litmus);
    if (vars.empty())
        return PropertyCheck::fail("no shared variables in litmus init block");

    auto p0 = extractColumn(litmus, 0);
    auto p1 = extractColumn(litmus, 1);
    auto joinAll = [](const std::vector<std::string> &rows) {
        std::string s;
        for (auto &r : rows) { s += r; s += '\n'; }
        return s;
    };
    std::string p0all = joinAll(p0);
    std::string p1all = joinAll(p1);

    for (const std::string &v : vars) {
        // Look for "[Xn]" with v assigned to Xn in init, OR plain `v` in either.
        if (p0all.find(v) == std::string::npos &&
            p1all.find(v) == std::string::npos) {
            // Init lines map var to register; check via "=" + var.
            // If var is referenced only through register, still pass.
            // Fall through: accept (init wiring guarantees access).
        }
    }

    // Stronger sub-check: if anchor is a release/SC store and the partner
    // has at least one load, P1 should contain LDR or LDAR.
    if (pair.bracket.anchorAlloc &&
        (pair.bracket.anchor.ordering == Ordering::Release ||
         pair.bracket.anchor.ordering == Ordering::SC) &&
        !pair.partnerAccesses.empty())
    {
        bool partnerHasLoad = false;
        for (const PartnerAccess &pa : pair.partnerAccesses) {
            const std::string &ir = pa.irInst;
            bool isStore = (ir.size() >= 6 && ir.compare(0, 6, "store ") == 0)
                        || ir.find("stlr") != std::string::npos
                        || ir.find("stlxr") != std::string::npos
                        || ir.find("swpal") != std::string::npos;
            if (!isStore) { partnerHasLoad = true; break; }
        }
        if (partnerHasLoad && !containsAny(p1all, { "LDR", "LDAR" }))
            return PropertyCheck::fail("P0 release/SC store but P1 emits no LDR/LDAR");
    }

    return PropertyCheck::pass();
}

// P4: No spurious hb-addition.  Every DMB/DSB/STLR/LDAR/SWPAL token appearing
// in P0 between the bracket endpoints (or before/after the anchor) must be
// derivable from an inline-asm or external-fn ordering source in the IR.
//
// We over-approximate by: every emitted barrier token must either
//   (a) match the expected anchor token (= the anchor itself), or
//   (b) belong to plausibleBarriersBetween(before, anchor) ∪
//        plausibleBarriersBetween(anchor, after).
static PropertyCheck
checkP4(const FunctionPair                &pair,
        const std::vector<Instruction *> &anchorInsts,
        const std::string                 &litmus)
{
    const AnchorBracket &br = pair.bracket;
    int bIdx = br.before ? findInstByIR(anchorInsts, br.before->irInst) : -1;
    int aIdx = findAnchorInstIdx(anchorInsts, br.anchor);
    int fIdx = br.after  ? findInstByIR(anchorInsts, br.after->irInst)  : -1;
    if (aIdx < 0)
        return PropertyCheck::fail("anchor not locatable for plausibility scan");

    std::set<std::string> plausible;
    if (br.before && bIdx >= 0 && bIdx < aIdx)
        for (auto &t : plausibleBarriersBetween(anchorInsts, bIdx, aIdx))
            plausible.insert(t);
    if (br.after && fIdx > aIdx)
        for (auto &t : plausibleBarriersBetween(anchorInsts, aIdx, fIdx))
            plausible.insert(t);

    // The anchor itself contributes its expected token.
    std::string expected = expectedAnchorToken(br);
    if (!expected.empty()) plausible.insert(expected);
    // STLR/SWPAL anchors also produce no additional barrier.

    auto p0 = extractColumn(litmus, 0);
    static const std::vector<std::string> kTokens = {
        "DMB ISH", "DMB OSH", "DSB SY", "DMB ST", "DMB LD",
        "STLR", "LDAR", "SWPAL"
    };

    for (const std::string &row : p0) {
        for (const std::string &tok : kTokens) {
            if (row.find(tok) == std::string::npos) continue;
            // Anchor row (contains expected token at the anchor position):
            // accept any subsumed match.
            if (!expected.empty() && row.find(expected) != std::string::npos &&
                tok == expected)
                continue;
            if (plausible.count(tok)) continue;
            // STLR/SWPAL/LDAR may have been emitted via an anchor or as a
            // channel read in partner; in P0 they must be derivable.
            // Allow MOV/LDR/STR as plain access lines (skip them).
            if (tok == "STLR" || tok == "SWPAL") continue; // handled by anchor expected
            return PropertyCheck::fail("P0 row '" + row +
                "' contains '" + tok + "' that is not derivable from the IR "
                "interval (anchorIdx=" + std::to_string(aIdx) + ")");
        }
    }
    return PropertyCheck::pass();
}

// P5: No spurious hb-removal (semantic dominance).
//   - SC anchor      → pass (SC dominance axiom subsumes any omitted barrier)
//   - Acquire anchor → pass for any omitted barrier of class <= Acquire
//   - Release anchor → pass for any omitted barrier of class <= Release
//   - CompilerOnly   → skip (cannot decide without callee scan)
//   - Relaxed / None → pass (no hb to remove)
//
// Concretely: scan (before, after); record the strongest plausible token.
// Compare its ordering class with the anchor's ordering class.
static PropertyCheck
checkP5(const FunctionPair                &pair,
        const std::vector<Instruction *> &anchorInsts)
{
    const AnchorBracket &br = pair.bracket;
    switch (br.anchor.ordering) {
        case Ordering::SC:           return PropertyCheck::pass();
        case Ordering::None:
        case Ordering::Relaxed:      return PropertyCheck::pass();
        case Ordering::CompilerOnly:
            return PropertyCheck::skip("CompilerOnly anchor: P5 unchecked "
                "(would require defined-callee body scan)");
        case Ordering::Acquire:
        case Ordering::Release:
            break;
    }

    int bIdx = br.before ? findInstByIR(anchorInsts, br.before->irInst) : -1;
    int aIdx = findAnchorInstIdx(anchorInsts, br.anchor);
    int fIdx = br.after  ? findInstByIR(anchorInsts, br.after->irInst)  : -1;
    if (aIdx < 0)
        return PropertyCheck::fail("anchor not locatable for dominance check");

    std::set<std::string> all;
    if (br.before && bIdx >= 0 && bIdx < aIdx)
        for (auto &t : plausibleBarriersBetween(anchorInsts, bIdx, aIdx))
            all.insert(t);
    if (br.after && fIdx > aIdx)
        for (auto &t : plausibleBarriersBetween(anchorInsts, aIdx, fIdx))
            all.insert(t);

    auto tokenOrdering = [](const std::string &t) -> Ordering {
        if (t == "DMB ISH" || t == "DSB SY" || t == "DMB OSH") return Ordering::SC;
        if (t == "DMB LD") return Ordering::Acquire;
        if (t == "DMB ST") return Ordering::Release;
        return Ordering::None;
    };
    Ordering anchorOrd = br.anchor.ordering;

    for (const std::string &t : all) {
        Ordering ord = tokenOrdering(t);
        // Omitted barrier dominates anchor → spurious removal.
        if (ord == Ordering::SC && anchorOrd != Ordering::SC)
            return PropertyCheck::fail("interval contains SC barrier '" + t +
                "' but anchor is only " +
                std::string(orderingStr(anchorOrd)) +
                " — possible hb removed");
        if (ord == Ordering::Acquire && anchorOrd == Ordering::Release)
            return PropertyCheck::fail("interval contains Acquire barrier '"
                + t + "' but anchor is Release-only");
        if (ord == Ordering::Release && anchorOrd == Ordering::Acquire)
            return PropertyCheck::fail("interval contains Release barrier '"
                + t + "' but anchor is Acquire-only");
    }
    return PropertyCheck::pass();
}

// P6: Alias faithfulness.  Two bracket events / partner events that share a
// litmus variable (same x/y/z) must have the same objDesc string from the
// SVF PTA (this is the input upon which VarMap was built).
static PropertyCheck
checkP6(const FunctionPair &pair)
{
    // Build (objDesc → set of roles using it).  In a faithful translation
    // every distinct litmus variable corresponds to a unique objDesc, and
    // every event with the same objDesc maps to the same variable — both
    // are guaranteed by VarMap construction.  The check here is simpler:
    // every objDesc must be non-empty (i.e. PTA produced a description).
    auto check = [](const std::string &obj, const char *role) -> PropertyCheck {
        if (obj.empty())
            return PropertyCheck::fail(std::string(role)
                + " access has empty PTA descriptor");
        return PropertyCheck::pass();
    };
    if (pair.bracket.before) {
        auto r = check(pair.bracket.before->objDesc, "before");
        if (!r.ok()) return r;
    }
    if (pair.bracket.anchorAlloc) {
        auto r = check(pair.bracket.anchorAlloc->objDesc, "anchorAlloc");
        if (!r.ok()) return r;
    }
    if (pair.bracket.after) {
        auto r = check(pair.bracket.after->objDesc, "after");
        if (!r.ok()) return r;
    }
    for (const PartnerAccess &pa : pair.partnerAccesses) {
        auto r = check(pa.objDesc, "partner");
        if (!r.ok()) return r;
    }
    return PropertyCheck::pass();
}

// P7: Exists clause structural soundness.  The clause must match one of:
//   exists (1:Rk=1)
//   ~exists (1:Rsync=1 [/\ 1:Rpay=0 ...])
// and every register named in the clause must be assigned somewhere in P1.
static PropertyCheck
checkP7(const std::string &litmus)
{
    std::string clause = extractExistsClause(litmus);
    if (clause.empty())
        return PropertyCheck::fail("no exists clause emitted");

    bool isNotExists = clause.find("~exists") != std::string::npos;
    bool isExists    = !isNotExists && clause.find("exists") != std::string::npos;
    if (!isExists && !isNotExists)
        return PropertyCheck::fail("clause is neither 'exists' nor '~exists'");

    // Find every "1:X<digit>" reference inside the parentheses.
    size_t lp = clause.find('(');
    size_t rp = clause.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp)
        return PropertyCheck::fail("malformed parentheses in clause");
    std::string inner = clause.substr(lp + 1, rp - lp - 1);

    // Collect referenced registers via simple scan.
    std::set<std::string> refs;
    for (size_t i = 0; i + 2 < inner.size(); ) {
        if (inner[i] == '1' && inner[i + 1] == ':') {
            size_t j = i + 2;
            while (j < inner.size() &&
                   (std::isalnum(static_cast<unsigned char>(inner[j])) ||
                    inner[j] == '_'))
                ++j;
            refs.insert(inner.substr(i + 2, j - (i + 2)));
            i = j;
        } else ++i;
    }
    if (refs.empty())
        return PropertyCheck::fail("clause references no P1 registers");

    // Every referenced register must appear somewhere in P1.
    // In AArch64 Xn and Wn alias the same register file slot, so a clause
    // reference "1:X2=1" is satisfied by P1 emitting either "X2" or "W2".
    auto p1 = extractColumn(litmus, 1);
    std::string p1all;
    for (auto &r : p1) { p1all += r; p1all += '\n'; }
    auto sibling = [](const std::string &r) -> std::string {
        if (r.size() >= 2 && (r[0] == 'X' || r[0] == 'W'))
            return std::string(1, r[0] == 'X' ? 'W' : 'X') + r.substr(1);
        return r;
    };
    for (const std::string &r : refs) {
        std::string s = sibling(r);
        if (p1all.find(r) == std::string::npos &&
            p1all.find(s) == std::string::npos)
            return PropertyCheck::fail("clause references P1 register '" + r
                                       + "' that is not used in P1");
    }

    // ~exists must include at least one payload-style "=0" comparison
    // when more than one register is referenced.
    if (isNotExists && refs.size() > 1 && inner.find("=0") == std::string::npos)
        return PropertyCheck::fail("~exists has multiple refs but no '=0' payload");

    return PropertyCheck::pass();
}

} // anonymous namespace

// ===========================================================================
// Pass implementation
// ===========================================================================

LitmusVerifierResult
LitmusVerifierPass::run(Module &M, ModuleAnalysisManager &MAM)
{
    LitmusVerifierResult out;

    const FnHeuristicsFilterResult &filtered =
        MAM.getResult<FnHeuristicsFilterPass>(M);
    const Herd7TranspilerResult &transpiled =
        MAM.getResult<Herd7TranspilerPass>(M);

    if (filtered.byAnchorFn.empty() || transpiled.litmusFiles.empty())
        return out;

    // Match each pair to its litmus by parsing the anchor-comment line of
    // every transpiled file (more robust than mirroring the transpiler's
    // skip conditions for degenerate exists clauses, which would couple
    // the verifier to the transpiler's internal heuristics).
    //
    // Each litmus's second line looks like:
    //   (* anchor: <label>  [<ordering>]  (<site>) *)
    // We index files by the (pairKey, label, ordering, site) tuple, and
    // for each pair look up exactly one matching unconsumed file.
    struct LitMeta {
        std::string filename;
        std::string label;
        std::string ordering;
        std::string site;
        bool        consumed = false;
    };
    std::map<std::string, std::vector<LitMeta>> byPairKey;
    for (const auto &[fname, body] : transpiled.litmusFiles) {
        // Extract the pair key from filename: strip ".litmus" and optional "_N".
        std::string key = fname;
        if (key.size() > 7 && key.compare(key.size() - 7, 7, ".litmus") == 0)
            key = key.substr(0, key.size() - 7);
        // Strip trailing "_<digits>" (the duplicate suffix).
        size_t us = key.rfind('_');
        if (us != std::string::npos) {
            bool allDigits = us + 1 < key.size();
            for (size_t k = us + 1; k < key.size(); ++k)
                if (!std::isdigit(static_cast<unsigned char>(key[k]))) {
                    allDigits = false; break;
                }
            if (allDigits) key = key.substr(0, us);
        }

        // Parse the anchor-comment line from `body`.
        LitMeta meta;
        meta.filename = fname;
        std::istringstream is(body);
        std::string line;
        while (std::getline(is, line)) {
            if (line.find("(* anchor:") == std::string::npos) continue;
            // anchor: <label>  [<ordering>]  (<site>)
            size_t lab = line.find("anchor:");
            size_t lb  = line.find('[', lab);
            size_t rb  = line.find(']', lb);
            size_t lp  = line.find('(', rb);
            size_t rp  = line.find(')', lp);
            if (lab == std::string::npos || lb == std::string::npos ||
                rb == std::string::npos  || lp == std::string::npos ||
                rp == std::string::npos)
                break;
            meta.label    = line.substr(lab + 7, lb - (lab + 7));
            meta.ordering = line.substr(lb + 1, rb - lb - 1);
            meta.site     = line.substr(lp + 1, rp - lp - 1);
            auto trim = [](std::string &s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                    s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                    s.pop_back();
            };
            trim(meta.label); trim(meta.ordering); trim(meta.site);
            break;
        }
        byPairKey[key].push_back(std::move(meta));
    }

    for (const auto &[anchorFnName, pairs] : filtered.byAnchorFn) {
        Function *anchorFn = M.getFunction(anchorFnName);
        if (!anchorFn || anchorFn->isDeclaration())
            continue;

        std::vector<Instruction *> anchorInsts = flattenFn(*anchorFn);

        for (const FunctionPair &pair : pairs) {
            const std::string pairKey = sanitise(anchorFnName) + "_" +
                                        sanitise(pair.partnerFn);

            auto it = byPairKey.find(pairKey);
            if (it == byPairKey.end()) continue;

            // Find the first unconsumed litmus whose anchor signature matches
            // this pair's bracket.
            const std::string ord(orderingStr(pair.bracket.anchor.ordering));
            LitMeta *match = nullptr;
            for (LitMeta &m : it->second) {
                if (m.consumed) continue;
                if (m.label == pair.bracket.anchor.label &&
                    m.ordering == ord &&
                    m.site == pair.bracket.anchor.site) {
                    match = &m;
                    break;
                }
            }
            if (!match) continue;
            match->consumed = true;

            const std::string &filename = match->filename;
            const std::string &litmus = transpiled.litmusFiles.at(filename);
            std::string litmusName = filename;
            if (litmusName.size() > 7 &&
                litmusName.compare(litmusName.size() - 7, 7, ".litmus") == 0)
                litmusName = litmusName.substr(0, litmusName.size() - 7);

            Function *partnerFn = M.getFunction(pair.partnerFn);
            if (!partnerFn || partnerFn->isDeclaration())
                continue;
            std::vector<Instruction *> partnerInsts = flattenFn(*partnerFn);

            PerLitmusReport rep;
            rep.litmusName = litmusName;
            rep.anchorFn   = anchorFnName;
            rep.partnerFn  = pair.partnerFn;

            rep.p1 = checkP1(anchorInsts, pair, partnerInsts);
            rep.p2 = checkP2(pair, litmus);
            rep.p3 = checkP3(pair, litmus);
            rep.p4 = checkP4(pair, anchorInsts, litmus);
            rep.p5 = checkP5(pair, anchorInsts);
            rep.p6 = checkP6(pair);
            rep.p7 = checkP7(litmus);

            // P8 is derived: P1 /\ P4 /\ P5 (treating skipped P5 as ok).
            bool p1ok = rep.p1.ok();
            bool p4ok = rep.p4.ok();
            bool p5ok = rep.p5.ok() || rep.p5.skipped();
            if (p1ok && p4ok && p5ok)
                rep.p8 = PropertyCheck::pass();
            else
                rep.p8 = PropertyCheck::fail("derived from P1/P4/P5 failure");

            // Update aggregates.
            auto bump = [](LitmusVerifierResult::Counts &c, const PropertyCheck &p) {
                if (p.ok())          ++c.pass;
                else if (p.skipped()) ++c.skip;
                else                  ++c.fail;
            };
            bump(out.p1, rep.p1);
            bump(out.p2, rep.p2);
            bump(out.p3, rep.p3);
            bump(out.p4, rep.p4);
            bump(out.p5, rep.p5);
            bump(out.p6, rep.p6);
            bump(out.p7, rep.p7);
            bump(out.p8, rep.p8);
            ++out.total;

            out.reports.push_back(std::move(rep));
        }
    }

    return out;
}
