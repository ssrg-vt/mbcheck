#include <filesystem>
#include <fstream>

#include "lib/anchor_pass.h"
#include "lib/causal_detect.h"
#include "lib/fn_heuristics.h"
#include "lib/function_pairs.h"
#include "lib/herd7_transpiler.h"
#include "lib/litmus_verifier.h"
#include "lib/pta.h"
#include "driver/mbcheck.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "Util/ExtAPI.h"
#include "lib/phase_timer.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace llvm;
using namespace std;

static cl::OptionCategory MBCheckCategory("MBCheck Analysis Options");

static cl::list<string> InputFiles(
    cl::Positional,
    cl::desc("<input bitcode/IR files>"),
    cl::OneOrMore,
    cl::cat(MBCheckCategory));

static cl::opt<bool> ShowAnchors(
    "anchors",
    cl::desc("List functions that contain memory-ordering anchors "
             "(inline asm + external calls). "
             "Sources: kernel-nb-anchor-apis-grouped.csv / ext-fn-ordering.csv."),
    cl::cat(MBCheckCategory));

static cl::opt<bool> ShowCausal(
    "causal",
    cl::desc("Run CausalDetect pass: for each anchor identify the last shared "
             "allocation access before it and the first after it "
             "(intraprocedural, uses SVF pointer analysis)."),
    cl::cat(MBCheckCategory));

static cl::opt<bool> ShowPairs(
    "pairs",
    cl::desc("Run FunctionPairs pass: for each anchor find partner functions "
             "that access the same shared objects, identifying both sides of "
             "the non-blocking pattern (uses SVF pointer analysis)."),
    cl::cat(MBCheckCategory));

static cl::opt<bool> ShowFilter(
    "filter",
    cl::desc("Run FnHeuristicsFilter pass: same as --pairs but removes pairs "
             "where the two functions are mutually exclusive (e.g. one calls "
             "the other in the call graph)."),
    cl::cat(MBCheckCategory));

static cl::opt<string> ShowLitmus(
    "litmus",
    cl::desc("Run Herd7TranspilerPass: for each surviving function pair generate "
             "an AArch64 herd7 .litmus file in the given output directory. "
             "Ordering instructions between shared accesses are included."),
    cl::value_desc("outdir"),
    cl::cat(MBCheckCategory));

static cl::opt<bool> ShowStats(
    "summary",
    cl::desc("Emit machine-parseable stats for aggregation by run scripts. "
             "Outputs STATS: lines covering anchors, causal brackets, "
             "fn-pairs (raw + filtered), and per-heuristic filter counts."),
    cl::cat(MBCheckCategory));

// Verifier (P1-P8 properties from src/spec/MBCheckIR.tla).
static cl::opt<bool> ShowVerify(
    "verify",
    cl::desc("Run LitmusVerifierPass: for every generated litmus test, "
             "check the eight representativeness properties P1-P8 of the "
             "reduced-IR abstraction against the original LLVM IR. "
             "Emits a per-test verdict and an aggregate summary."),
    cl::cat(MBCheckCategory));

// T2.1 — Heuristic ablation / differential mode.
// Names: BenignAnchor, ReadRead, CallGraph, LockAlias, Lifecycle,
// RcuSerial, CompletionOrder, IrqSerial, SubsystemLock, PerCpu,
// SafeAllocation.  Special token "all" disables every heuristic.
// Hits are still counted (so STATS:heuristic_hits remains meaningful)
// but disabled heuristics do not actually filter the pair out.
static cl::list<std::string> DisableHeuristic(
    "disable-heuristic",
    cl::desc("Disable one or more pair-filter heuristics (may be repeated). "
             "Pair counts still appear in STATS but pairs are NOT filtered. "
             "Use 'all' to disable every heuristic."),
    cl::value_desc("name"),
    cl::ZeroOrMore,
    cl::CommaSeparated,
    cl::cat(MBCheckCategory));

// ---------------------------------------------------------------------------
// Shared formatting helpers used by multiple run* functions
// ---------------------------------------------------------------------------
static const char *ordTag(anchors::Ordering o)
{
    switch (o) {
    case anchors::Ordering::SC:           return "[SC]           ";
    case anchors::Ordering::Acquire:      return "[acquire]      ";
    case anchors::Ordering::Release:      return "[release]      ";
    case anchors::Ordering::Relaxed:      return "[relaxed]      ";
    case anchors::Ordering::CompilerOnly: return "[compiler-only]";
    default:                              return "[none]         ";
    }
}

static const char *accessKindStr(SharedAccess::Kind k)
{
    return k == SharedAccess::Kind::Load ? "load " : "store";
}

// Print one FunctionPair in the standard human-readable format.
static void printOnePair(const FunctionPair &p)
{
    const AnchorBracket &br = p.bracket;

    cout << "Fn-pairs: {" << p.anchorFn << ", " << p.partnerFn << " }\n";
    cout << p.anchorFn << ":\n";

    std::string anchorTag = br.anchorAlloc
        ? "[" + br.anchorAlloc->ptrDesc + "]"
        : "[" + br.anchor.label + "]";
    cout << "  Anchor: " << anchorTag
         << "  " << br.anchor.label
         << "  (" << br.anchor.site << ")\n";

    if (br.before)
        cout << "  Before:  [" << br.before->objDesc << "]  "
             << br.before->irInst << "\n";
    else
        cout << "  Before: none\n";

    if (br.after)
        cout << "  After:   [" << br.after->objDesc << "]  "
             << br.after->irInst << "\n";
    else
        cout << "  After: none\n";

    cout << "\n";

    cout << p.partnerFn << "\n";
    cout << "  Accesses:\n";
    for (const PartnerAccess &pa : p.partnerAccesses)
        cout << "    [" << pa.objDesc << "]  " << pa.irInst << "\n";

    cout << "\n";
}

static void runPTAPass(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return FSPTAPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const FSPTAResult &result =
        MAM.getResult<FSPTAPass>(M);

    vector<FSPTAResult::NodeId> objIds;
    objIds.reserve(result.reversePointsTo.size());
    
    for (auto &[objId, unused] : result.reversePointsTo)
        objIds.push_back(objId);
    std::sort(objIds.begin(), objIds.end());

    for (auto objId : objIds)
    {
        auto descIt = result.nodeDescriptions.find(objId);
        cout << "Pointee [" << objId << "]: "
             << (descIt != result.nodeDescriptions.end()
                     ? descIt->second : "(unknown)") << "\n";

        vector<FSPTAResult::NodeId> ptrs = result.reversePointsTo.at(objId);
        std::sort(ptrs.begin(), ptrs.end());
        for (auto ptrId : ptrs)
        {
            auto pDescIt = result.nodeDescriptions.find(ptrId);
            cout << "  Pointer [" << ptrId << "]: "
                 << (pDescIt != result.nodeDescriptions.end()
                         ? pDescIt->second : "(unknown)") << "\n";
        }
        cout << "\n";
    }
}

static void runAnchorPass(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const AnchorResult &result = MAM.getResult<AnchorPass>(M);

    if (result.perFunction.empty()) {
        cout << "(no anchors found)\n";
        return;
    }

    for (const auto &[fnName, hits] : result.perFunction) {
        cout << fnName << ":\n";
        for (const AnchorHit &h : hits) {
            cout << "  " << ordTag(h.ordering) << "  " << h.label;
            if (!h.asmInst.empty())
                cout << "  \u2192  " << h.asmInst;
            cout << "  (" << h.site << ")\n";
        }
    }
}

static void runCausalDetect(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const CausalDetectResult &result = MAM.getResult<CausalDetectPass>(M);

    if (result.perFunction.empty()) {
        cout << "(no causal brackets found)\n";
        return;
    }

    auto printAccess = [](const SharedAccess &a, const char *tag) {
        cout << "  " << tag << " [" << accessKindStr(a.kind) << "]  "
             << a.irInst << "\n";
    };

    for (const auto &[fnName, brackets] : result.perFunction) {
        cout << fnName << ":\n";
        for (const AnchorBracket &br : brackets) {
            // ── before ──────────────────────────────────────────────────────
            if (br.before)
                printAccess(*br.before, "before");
            else
                cout << "  before: (none)\n";

            // ── anchor (+ associated allocation) ────────────────────────────
            cout << "  anchor: " << ordTag(br.anchor.ordering)
                 << "  " << br.anchor.label
                 << "  (" << br.anchor.site << ")\n";
            if (br.anchorAlloc)
                cout << "          alloc: " << br.anchorAlloc->ptrDesc << "\n";

            // ── after ───────────────────────────────────────────────────────
            if (br.after)
                printAccess(*br.after, "after ");
            else
                cout << "  after:  (none)\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Shared helper: print a map of FunctionPair lists in the standard format.
// Used by both --pairs and --filter.
// ---------------------------------------------------------------------------
static void printPairs(const std::map<std::string, std::vector<FunctionPair>> &byAnchorFn)
{
    for (const auto &[anchorFn, pairs] : byAnchorFn)
        for (const FunctionPair &p : pairs)
            printOnePair(p);
}

static void runStats(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    MAM.registerPass([] { return FunctionPairsPass(); });
    MAM.registerPass([] { return FnHeuristicsFilterPass(); });
    MAM.registerPass([] { return Herd7TranspilerPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const std::string div80(80, '=');
    const std::string div40(40, '-');

    // ── Run all passes ─────────────────────────────────────────────────── 
    const AnchorResult           &anchors  = MAM.getResult<AnchorPass>(M);
    const CausalDetectResult     &causal   = MAM.getResult<CausalDetectPass>(M);
    // FnHeuristicsFilterPass internally requests FunctionPairsPass;
    // request filter first so the raw pairs result is cached for retrieval.
    const FnHeuristicsFilterResult &filtered =
        MAM.getResult<FnHeuristicsFilterPass>(M);
    const FunctionPairsResult      &rawPairs = MAM.getResult<FunctionPairsPass>(M);
    const Herd7TranspilerResult    &litmus   = MAM.getResult<Herd7TranspilerPass>(M);

    // ── Aggregate counts ─────────────────────────────────────────────────
    int totalAnchors = 0;
    std::map<std::string, int> anchorLabelCounts;
    for (const auto &[fn, hits] : anchors.perFunction)
        for (const AnchorHit &h : hits) {
            ++totalAnchors;
            ++anchorLabelCounts[h.label];
        }

    int totalCausal = 0;
    for (const auto &[fn, brackets] : causal.perFunction)
        totalCausal += static_cast<int>(brackets.size());

    int survivingPairs = 0;
    for (const auto &[fn, plist] : filtered.byAnchorFn)
        survivingPairs += static_cast<int>(plist.size());

    int totalLitmus = static_cast<int>(litmus.litmusFiles.size());

    // ── Machine-parseable STATS: lines (consumed by run_kernel_noLTO.py) ─
    cout << "STATS:anchors:total=" << totalAnchors << "\n";
    for (const auto &[label, cnt] : anchorLabelCounts)
        cout << "STATS:anchor_label:" << label << "=" << cnt << "\n";
    cout << "STATS:causal:total=" << totalCausal << "\n";
    cout << "STATS:pairs:total=" << filtered.totalPairs << "\n";
    cout << "STATS:pairs:filtered=" << (filtered.totalPairs - survivingPairs) << "\n";
    cout << "STATS:pairs:surviving=" << survivingPairs << "\n";
    for (const auto &[hname, cnt] : filtered.heuristicHits)
        cout << "STATS:heuristic:" << hname << "=" << cnt << "\n";
    for (const auto &[hname, subcats] : filtered.heuristicSubHits)
        for (const auto &[subcat, cnt] : subcats)
            cout << "STATS:heuristic_sub:" << hname << ":" << subcat << "=" << cnt << "\n";
    cout << "STATS:litmus:total=" << totalLitmus << "\n";

    // ════════════════════════════════════════════════════════════════════
    // Verbose output — anchors, causal, pairs (raw), pairs (surviving), 
    // pairs (filtered with heuristic attribution).  Written to stdout so
    // the run script captures it into the per-file log section.
    // ════════════════════════════════════════════════════════════════════

    // ── PASS: ANCHORS ────────────────────────────────────────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: ANCHORS  (" << totalAnchors << " anchors in "
         << anchors.perFunction.size() << " functions)\n";
    cout << div80 << "\n";
    if (anchors.perFunction.empty()) {
        cout << "(no anchors found)\n";
    } else {
        for (const auto &[fnName, hits] : anchors.perFunction) {
            cout << fnName << ":\n";
            for (const AnchorHit &h : hits) {
                cout << "  " << ordTag(h.ordering) << "  " << h.label;
                if (!h.asmInst.empty())
                    cout << "  ->  " << h.asmInst;
                cout << "  (" << h.site << ")\n";
            }
        }
    }

    // ── PASS: CAUSAL ─────────────────────────────────────────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: CAUSAL  (" << totalCausal << " brackets in "
         << causal.perFunction.size() << " functions)\n";
    cout << div80 << "\n";
    if (causal.perFunction.empty()) {
        cout << "(no causal brackets found)\n";
    } else {
        auto printAccess = [](const SharedAccess &a, const char *tag) {
            cout << "  " << tag << " [" << accessKindStr(a.kind) << "]  "
                 << a.irInst << "\n";
        };
        for (const auto &[fnName, brackets] : causal.perFunction) {
            cout << fnName << ":\n";
            for (const AnchorBracket &br : brackets) {
                if (br.before)  printAccess(*br.before, "before");
                else            cout << "  before: (none)\n";
                cout << "  anchor: " << ordTag(br.anchor.ordering)
                     << "  " << br.anchor.label
                     << "  (" << br.anchor.site << ")\n";
                if (br.anchorAlloc)
                    cout << "          alloc: " << br.anchorAlloc->ptrDesc << "\n";
                if (br.after)   printAccess(*br.after, "after ");
                else            cout << "  after:  (none)\n";
            }
        }
    }

    // ── PASS: PAIRS RAW ──────────────────────────────────────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: PAIRS RAW  (" << filtered.totalPairs << " pairs)\n";
    cout << div80 << "\n";
    if (rawPairs.byAnchorFn.empty()) {
        cout << "(no function pairs found)\n";
    } else {
        printPairs(rawPairs.byAnchorFn);
    }

    // ── PASS: PAIRS SURVIVING (post-filter) ──────────────────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: PAIRS SURVIVING  (" << survivingPairs << " of "
         << filtered.totalPairs << " pairs, "
         << (filtered.totalPairs - survivingPairs) << " filtered)\n";
    cout << div80 << "\n";
    if (filtered.byAnchorFn.empty()) {
        cout << "(no concurrent function pairs found)\n";
    } else {
        printPairs(filtered.byAnchorFn);
    }

    // ── PASS: PAIRS FILTERED (with heuristic attribution) ────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: PAIRS FILTERED  ("
         << (filtered.totalPairs - survivingPairs) << " pairs)\n";
    cout << div80 << "\n";
    if (filtered.filteredRecords.empty()) {
        cout << "(no pairs were filtered)\n";
    } else {
        for (const FilteredPairRecord &rec : filtered.filteredRecords) {
            cout << div40 << "\n";
            cout << "Heuristic: " << rec.heuristicName << "\n";
            printOnePair(rec.pair);
        }
    }

    // ── PASS: HERD7 LITMUS ───────────────────────────────────────────────
    cout << "\n" << div80 << "\n";
    cout << "PASS: HERD7 LITMUS  (" << totalLitmus << " litmus file"
         << (totalLitmus == 1 ? "" : "s") << " from "
         << survivingPairs << " surviving pair"
         << (survivingPairs == 1 ? "" : "s") << ")\n";
    cout << div80 << "\n";
    if (litmus.litmusFiles.empty()) {
        cout << "(no litmus files generated)\n";
    } else {
        for (const auto &[filename, text] : litmus.litmusFiles)
            cout << "  " << filename << "\n";
    }
}

static void runFunctionPairs(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    MAM.registerPass([] { return FunctionPairsPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const FunctionPairsResult &result = MAM.getResult<FunctionPairsPass>(M);

    if (result.byAnchorFn.empty()) {
        cout << "(no function pairs found)\n";
        return;
    }
    printPairs(result.byAnchorFn);
}

static void runFilteredPairs(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    MAM.registerPass([] { return FunctionPairsPass(); });
    MAM.registerPass([] { return FnHeuristicsFilterPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const FnHeuristicsFilterResult &result =
        MAM.getResult<FnHeuristicsFilterPass>(M);

    if (result.byAnchorFn.empty()) {
        cout << "(no concurrent function pairs found)\n";
        return;
    }
    printPairs(result.byAnchorFn);
}

static void runHerd7Transpiler(Module &M, const string &outDir)
{
    namespace fs = std::filesystem;

    // Create output directory if it doesn't exist.
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        llvm::errs() << "Error: cannot create output directory '" << outDir
                     << "': " << ec.message() << "\n";
        return;
    }

    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    MAM.registerPass([] { return FunctionPairsPass(); });
    MAM.registerPass([] { return FnHeuristicsFilterPass(); });
    MAM.registerPass([] { return Herd7TranspilerPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const Herd7TranspilerResult &result =
        MAM.getResult<Herd7TranspilerPass>(M);

    if (result.litmusFiles.empty()) {
        cout << "(no concurrent function pairs — no litmus files generated)\n";
        return;
    }

    int written = 0;
    for (const auto &[filename, text] : result.litmusFiles) {
        fs::path outPath = fs::path(outDir) / filename;
        std::ofstream ofs(outPath);
        if (!ofs) {
            llvm::errs() << "Error: cannot write '" << outPath.string() << "'\n";
            continue;
        }
        ofs << text;
        cout << "  wrote: " << outPath.string() << "\n";
        ++written;
    }
    cout << "Generated " << written << " litmus file"
         << (written == 1 ? "" : "s") << " in " << outDir << "\n";
}

static void runVerifier(Module &M)
{
    ModuleAnalysisManager MAM;
    MAM.registerPass([] { return AnchorPass(); });
    MAM.registerPass([] { return CausalDetectPass(); });
    MAM.registerPass([] { return FunctionPairsPass(); });
    MAM.registerPass([] { return FnHeuristicsFilterPass(); });
    MAM.registerPass([] { return Herd7TranspilerPass(); });
    MAM.registerPass([] { return LitmusVerifierPass(); });
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);

    const LitmusVerifierResult &res = MAM.getResult<LitmusVerifierPass>(M);

    if (res.reports.empty()) {
        cout << "(no litmus tests to verify)\n";
        return;
    }

    // ── Per-test verdicts ────────────────────────────────────────────────
    auto tag = [](const PropertyCheck &p) -> const char * {
        if (p.ok())          return "PASS";
        if (p.skipped())     return "SKIP";
        return "FAIL";
    };
    for (const PerLitmusReport &r : res.reports) {
        cout << r.litmusName << "  ("
             << r.anchorFn << " | " << r.partnerFn << ")\n";
        cout << "  P1=" << tag(r.p1)
             << "  P2=" << tag(r.p2)
             << "  P3=" << tag(r.p3)
             << "  P4=" << tag(r.p4)
             << "  P5=" << tag(r.p5)
             << "  P6=" << tag(r.p6)
             << "  P7=" << tag(r.p7)
             << "  P8=" << tag(r.p8)
             << "\n";
        auto warn = [&](const char *name, const PropertyCheck &p) {
            if (p.failed())
                cout << "    " << name << " FAIL: " << p.detail << "\n";
            else if (p.skipped() && !p.detail.empty())
                cout << "    " << name << " SKIP: " << p.detail << "\n";
        };
        warn("P1", r.p1); warn("P2", r.p2); warn("P3", r.p3); warn("P4", r.p4);
        warn("P5", r.p5); warn("P6", r.p6); warn("P7", r.p7); warn("P8", r.p8);
    }

    // ── Aggregate summary ────────────────────────────────────────────────
    auto pct = [&](int n) {
        if (res.total == 0) return std::string("0.0%");
        std::ostringstream o;
        o.precision(1);
        o << std::fixed << (100.0 * n / res.total) << "%";
        return o.str();
    };
    cout << "\n" << std::string(72, '=') << "\n";
    cout << "VERIFIER SUMMARY  (" << res.total << " litmus tests)\n";
    cout << std::string(72, '=') << "\n";
    auto row = [&](const char *name, const LitmusVerifierResult::Counts &c) {
        cout << "  " << name
             << "  pass=" << c.pass << " (" << pct(c.pass) << ")"
             << "  fail=" << c.fail
             << "  skip=" << c.skip << "\n";
    };
    row("P1 PO-embedding         ", res.p1);
    row("P2 Anchor fidelity      ", res.p2);
    row("P3 rf-closure           ", res.p3);
    row("P4 No spurious hb-add   ", res.p4);
    row("P5 No spurious hb-remove", res.p5);
    row("P6 Alias faithfulness   ", res.p6);
    row("P7 Exists soundness     ", res.p7);
    row("P8 Conservative approx  ", res.p8);

    // ── Machine-parseable line for sweep-script aggregation ──────────────
    cout << "STATS:verify:total=" << res.total << "\n";
    auto stats = [&](const char *name, const LitmusVerifierResult::Counts &c) {
        cout << "STATS:verify:" << name
             << ":pass=" << c.pass
             << ":fail=" << c.fail
             << ":skip=" << c.skip << "\n";
    };
    stats("p1", res.p1); stats("p2", res.p2); stats("p3", res.p3);
    stats("p4", res.p4); stats("p5", res.p5); stats("p6", res.p6);
    stats("p7", res.p7); stats("p8", res.p8);
}

int main(int argc, char **argv)
{
    cl::HideUnrelatedOptions(MBCheckCategory);
    cl::ParseCommandLineOptions(argc, argv,
        "MBCheck Analysis: pointee -> pointers\n"
        "Loads IR with LLVM, analyses with SVF FlowSensitive WPA\n");

    // T2.1 — push ablation list to the heuristic filter pass.
    {
        std::set<std::string> dis;
        static const std::array<const char*, 11> kAllNames = {
            "BenignAnchor", "ReadRead", "CallGraph", "LockAlias",
            "Lifecycle", "RcuSerial", "CompletionOrder", "IrqSerial",
            "SubsystemLock", "PerCpu", "SafeAllocation"
        };
        for (const auto &n : DisableHeuristic) {
            if (n == "all") {
                for (auto *a : kAllNames) dis.insert(a);
            } else {
                dis.insert(n);
            }
        }
        if (!dis.empty()) {
            errs() << "[mbcheck] ablation: disabling heuristics =";
            for (const auto &n : dis) errs() << " " << n;
            errs() << "\n";
        }
        setDisabledHeuristics(std::move(dis));
    }

    // extapi.bc: $MBCHECK_EXTAPI_BC, then $SVF_DIR (SVF's own lookup), then
    // the path recorded at configure time.
    if (const char *p = std::getenv("MBCHECK_EXTAPI_BC")) {
        if (!SVF::ExtAPI::setExtBcPath(p))
            errs() << "[mbcheck] WARNING: MBCHECK_EXTAPI_BC is not readable: "
                   << p << "\n";
    } else if (!std::getenv("SVF_DIR")) {
#ifdef MBCHECK_SVF_EXTAPI_BC
        SVF::ExtAPI::setExtBcPath(MBCHECK_SVF_EXTAPI_BC);
#endif
    }

    llvm_shutdown_obj SDO;

    LLVMContext ctx;
    SMDiagnostic err;

    mbtime::Scope *tLoad = new mbtime::Scope("ir_load");
    unique_ptr<Module> mergedIR = parseIRFile(InputFiles[0], err, ctx);
    if (!mergedIR)
    {
        err.print(argv[0], llvm::errs());
        return 1;
    }

    if (InputFiles.size() > 1)
    {
        Linker linker(*mergedIR);
        for (unsigned i = 1; i < InputFiles.size(); ++i)
        {
            unique_ptr<Module> m = parseIRFile(InputFiles[i], err, ctx);
            if (!m)
            {
                err.print(argv[0], llvm::errs());
                return 1;
            }
            if (linker.linkInModule(std::move(m)))
            {
                llvm::errs() << "Error: failed to link " << InputFiles[i] << "\n";
                return 1;
            }
        }
    }

    delete tLoad;

    {
        mbtime::Scope tOther("other");
    if (ShowAnchors)
        runAnchorPass(*mergedIR);
    else if (ShowCausal)
        runCausalDetect(*mergedIR);
    else if (ShowPairs)
        runFunctionPairs(*mergedIR);
    else if (ShowFilter)
        runFilteredPairs(*mergedIR);
    else if (!ShowLitmus.empty())
        runHerd7Transpiler(*mergedIR, ShowLitmus);
    else if (ShowVerify)
        runVerifier(*mergedIR);
    else if (ShowStats)
        runStats(*mergedIR);
    else
        runPTAPass(*mergedIR);
    }

    mbtime::printStats();
    return 0;
}
