#!/usr/bin/env python3
"""Run mbcheck over a directory of single-module IR files.

A case is TN when no litmus test reaches its forbidden state, either because
the pair was filtered or because herd7 answered Ok.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import herdtools  # noqa: E402


def herd7_verdict(herd7, herdlib, litmus, timeout=120):
    try:
        proc = subprocess.run(herdtools.argv(herd7, herdlib, litmus),
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    for line in proc.stdout.splitlines():
        s = line.strip()
        if s == "Ok":
            return True
        if s == "No":
            return False
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mbcheck", type=Path, required=True)
    ap.add_argument("--irdir",   type=Path, required=True)
    ap.add_argument("--outdir",  type=Path, required=True)
    ap.add_argument("--herd7",   type=Path, default=None)
    ap.add_argument("--herdlib", type=Path, default=None)
    ap.add_argument("--json",    type=Path, required=True)
    args = ap.parse_args()

    herd7 = herdtools.resolve_herd7(args.herd7)
    if herd7 is None or not herd7.is_file():
        sys.exit("herd7 not found.  Pass --herd7, set $HERD7, or put herd7 on "
                 "$PATH.  See README.md.")
    herdlib = herdtools.resolve_herdlib(herd7, args.herdlib)
    # Otherwise the oracle fails per test and every case looks like a finding.
    problem = herdtools.probe(herd7, herdlib)
    if problem:
        sys.exit(f"{problem}\n  See README.md.")

    ir_files = sorted(args.irdir.glob("*.ll"))
    if not ir_files:
        sys.exit(f"no .ll files in {args.irdir}")

    cases = []
    for ll in ir_files:
        name = ll.stem
        work = args.outdir / name
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)

        proc = subprocess.run(
            [str(args.mbcheck), "--litmus", str(work), str(ll)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=600)

        litmus = sorted(work.glob("*.litmus"))
        ok = no = err = 0
        offenders = []
        for lf in litmus:
            v = herd7_verdict(herd7, herdlib, lf)
            if v is True:
                ok += 1
            elif v is False:
                no += 1
                offenders.append(lf.name)
            else:
                err += 1

        cases.append({
            "case":      name,
            "tool_rc":   proc.returncode,
            "litmus":    len(litmus),
            "ok":        ok,
            "no":        no,
            "err":       err,
            # herd7 not answering is a broken setup, not a finding.
            "verdict":   ("ERROR" if (err or proc.returncode != 0)
                          else "TN" if no == 0 else "FP"),
            "offenders": offenders,
        })
        print(f"  {name:<24} litmus={len(litmus):<4} Ok={ok:<4} No={no:<3} "
              f"-> {cases[-1]['verdict']}", flush=True)

    summary = {
        "irdir": str(args.irdir),
        "cases": cases,
        "total": len(cases),
        "tn":    sum(1 for c in cases if c["verdict"] == "TN"),
        "fp":    sum(1 for c in cases if c["verdict"] == "FP"),
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(summary, indent=2))
    print(f"\n  {summary['tn']} TN, {summary['fp']} FP of {summary['total']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
