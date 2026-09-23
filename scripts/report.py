#!/usr/bin/env python3
"""Print the measured results for a claim.

Usage: report.py <claim> <results.json>
"""

import argparse
import json
import sys
from pathlib import Path

TITLES = {
    "smoke":      "Smoke test (50 modules, v6.19)",
    "table4":     "Table 4: kernel sweep, Linux v6.19",
    "table5":     "Table 5: runtime breakdown",
    "table6":     "Table 6: top anchors and external functions",
    "table7":     "Table 7: heuristic hits",
    "table8":     "Table 8: microbenchmark suite",
    "table9":     "Table 9: violations by module, Linux v6.19",
}

# label -> dotted path into the results JSON
METRICS = {
    "smoke": [
        ("Modules analysed",             "ir_modules"),
        ("Anchors",                      "anchors"),
        ("Causal brackets",              "causal"),
        ("Fn pairs (raw)",               "pairs_total"),
        ("Fn pairs surviving",           "pairs_surviving"),
        ("Litmus synthesised",           "litmus"),
        ("herd7 Ok",                     "herd7_ok"),
        ("herd7 No",                     "herd7_no"),
        ("herd7 errors",                 "herd7_err"),
    ],
    "table4": [
        ("IR modules",                   "ir_modules"),
        ("IR lines",                     "ir_lines"),
        ("Anchors",                      "anchors"),
        ("Causal brackets",              "causal"),
        ("Fn pairs (before heuristics)", "pairs_total"),
        ("Fn pairs (after heuristics)",  "pairs_surviving"),
        ("Litmus synthesised",           "litmus"),
        ("herd7 violations",             "herd7_no"),
    ],
    "table5": [
        ("Summarize ext-fn (%)",           "timing.shares.summarize_extfn"),
        ("Anchor database (%)",            "timing.shares.anchor_db"),
        ("Bracket extraction (%)",         "timing.shares.bracket_extraction"),
        ("Function pairing (%)",           "timing.shares.function_pairing"),
        ("Mutual exclusion filtering (%)", "timing.shares.mutex_filtering"),
        ("Transpiler + verifier (%)",      "timing.shares.transpiler"),
        ("Herd7 analysis (%)",             "timing.shares.herd7"),
        ("IR parse / link (%)",            "timing.shares.ir_load"),
        ("Other setup / teardown (%)",     "timing.shares.other"),
        ("Wall clock (s)",                 "timing.wall_total_s"),
        ("Peak RSS (GB)",                  "timing.peak_rss_gb"),
        ("CPU, all children (s)",          "timing.cpu_child_s"),
        ("CPU attributed to stages (s)",   "timing.cpu_attributed_s"),
        ("CPU in timed-out modules (s)",   "timing.cpu_timeout_s"),
    ],
    "table6": [
        ("dmb oshld  (acquire)",     "anchor_labels.dmb oshld"),
        ("dmb oshst  (release OSH)", "anchor_labels.dmb oshst"),
        ("stlxr      (release)",     "anchor_labels.stlxr"),
        ("dmb ish    (SC)",          "anchor_labels.dmb ish"),
        ("_raw_spin_lock",           "anchor_labels._raw_spin_lock (ext)"),
        ("__kmalloc_cache_noprof",   "anchor_labels.__kmalloc_cache_noprof (ext)"),
        ("__kmalloc_noprof",         "anchor_labels.__kmalloc_noprof (ext)"),
        ("flush_work",               "anchor_labels.flush_work (ext)"),
    ],
    "table7": [
        ("CallGraph",           "heuristics.CallGraph"),
        ("Lifecycle",           "heuristics.Lifecycle"),
        ("LockAlias",           "heuristics.LockAlias"),
        ("SafeAllocation",      "heuristics.SafeAllocation"),
        ("ReadRead",            "heuristics.ReadRead"),
        ("BenignAnchor",        "heuristics.BenignAnchor"),
        ("Candidate pairs",     "pairs_total"),
    ],
}


def resolve(data, path):
    cur = data
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:,.2f}"
    if isinstance(v, int):
        return f"{v:,}"
    return str(v)


def print_metrics(claim, data):
    rows = METRICS[claim]
    w = max(len(label) for label, _ in rows) + 4
    for label, path in rows:
        print(f"  {label:<{w}}{fmt(resolve(data, path)):>14}")


def print_violations(data):
    counts = {}
    for v in data.get("violations", []):
        counts[v["module"]] = counts.get(v["module"], 0) + 1
    if not counts:
        print("  none")
        return
    w = max(len(m) for m in counts) + 4
    for module in sorted(counts, key=lambda m: (-counts[m], m)):
        print(f"  {module:<{w}}{counts[module]:>7}")
    print(f"  {'':<{w}}{'':>7}")
    print(f"  {'Total':<{w}}{sum(counts.values()):>7}")
    print(f"  {'Modules':<{w}}{len(counts):>7}")


def print_suite(data):
    cases = data["cases"]
    w = max(len(c["case"]) for c in cases) + 4
    print(f"  {'Case':<{w}}{'Litmus':>8}{'Flagged':>9}{'Verdict':>9}")
    for c in cases:
        print(f"  {c['case']:<{w}}{c['litmus']:>8}{c['no']:>9}{c['verdict']:>9}")
    print()
    print(f"  {data['tn']} TN, {data['fp']} FP of {data['total']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("claim")
    ap.add_argument("results", type=Path)
    args = ap.parse_args()

    data = json.loads(args.results.read_text())

    print()
    print(f"  {TITLES.get(args.claim, args.claim)}")
    print()

    if args.claim == "table9":
        print_violations(data)
    elif args.claim == "table8":
        print_suite(data)
    elif args.claim in METRICS:
        print_metrics(args.claim, data)
    else:
        sys.exit(f"unknown claim: {args.claim}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
