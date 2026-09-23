#!/usr/bin/env python3
"""Patch synthesis for litmus tests herd7 answers No.

Tries candidate fixes in order of cost, re-running herd7 after each, and
reports the first that makes the forbidden state unreachable:

  promote     flag store STR -> STLR, flag load LDR -> LDAR
  wmb-rmb     DMB ST before the store, DMB LD after the load
  mb-mb       DMB ISH at both
  promote+mb  both of the above

The flag variable comes from the exists clause.  Writes <litmus>.patch.txt
next to each No test.

  mb_patch.py <litmus-dir> [--ll <module.ll>] [--herd7 BIN] [--herdlib DIR]
              [--retries N] [--only-no] [--json FILE]
"""

import argparse
import json
import os
import shutil
import re
import subprocess
import sys
from pathlib import Path

def _default_herd7():
    """$HERD7, else the first herd7 on $PATH."""
    env = os.environ.get("HERD7")
    if env:
        return env
    for name in ("herd7", "herd.exe"):
        found = shutil.which(name)
        if found:
            return found
    return "herd7"


def _default_herdlib(herd7):
    """$HERDLIB, else derived from the binary location."""
    env = os.environ.get("HERDLIB")
    if env:
        return env
    try:
        binpath = Path(herd7).resolve()
    except OSError:
        return ""
    for parent in binpath.parents:
        for rel in ("herd/libdir", "share/herdtools7/herd/libdir"):
            cand = parent / rel
            if (cand / "stdlib.cat").is_file():
                return str(cand)
    return ""


HERD7   = _default_herd7()
HERDLIB = _default_herdlib(HERD7)


def run_herd7(path, herd7=HERD7, herdlib=HERDLIB, timeout=60):
    """Return ('Ok'|'No'|'ERR', observation-line)."""
    try:
        p = subprocess.run([herd7, "-set-libdir", herdlib, str(path)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "ERR", "timeout"
    verdict, obs = "ERR", ""
    for line in p.stdout.splitlines():
        s = line.strip()
        if s in ("Ok", "No"):
            verdict = s
        elif s.startswith("Observation"):
            obs = s
    return verdict, obs


class Litmus:
    """Minimal structural view of a two-thread MBCheck litmus file."""

    def __init__(self, text):
        self.text = text
        self.lines = text.splitlines()
        self.header = [l for l in self.lines if l.startswith("(*")]
        m = re.search(r"anchor-fn:\s*(\S+)\s+partner-fn:\s*(\S+)", text)
        self.anchor_fn, self.partner_fn = (m.group(1), m.group(2)) if m else ("?", "?")
        m = re.search(r"\(\* anchor:\s*(.*?)\s*\*\)", text)
        self.anchor_desc = m.group(1) if m else ""
        # init block: "0:X1=y;" / "1:X0=x;"
        self.addr = {}  # (tid, 'X1') -> var
        for tid, reg, var in re.findall(r"(\d):(X\d+)=(\w+);", text):
            self.addr[(int(tid), reg)] = var
        # body rows
        self.body_start = next(i for i, l in enumerate(self.lines) if re.match(r"\s*P0\s*\|", l)) + 1
        self.body_end = next(i for i, l in enumerate(self.lines) if "exists" in l and not l.startswith("(*"))
        self.rows = []  # list of [p0, p1]
        for l in self.lines[self.body_start:self.body_end]:
            if "|" not in l:
                continue
            p0, p1 = l.split("|", 1)
            self.rows.append([p0.strip().rstrip(";").strip(), p1.strip().rstrip(";").strip()])
        self.exists = self.lines[self.body_end]

    def flag_var(self):
        m = re.search(r"1:([XW])(\d+)=1", self.exists)
        if not m:
            return None, None, None
        k = m.group(2)
        # P1 row that defines W<k>/X<k> via a load
        for ri, (_, p1) in enumerate(self.rows):
            mm = re.match(r"(LDR|LDAR)\s+[WX]%s\s*,\s*\[(X\d+)" % k, p1)
            if mm:
                var = self.addr.get((1, mm.group(2)))
                return var, ri, mm.group(1)
        return None, None, None

    def p0_store_row(self, var):
        """Last P0 row that stores to `var` (STR/STLR/SWP*)."""
        regs = [r for (tid, r), v in self.addr.items() if tid == 0 and v == var]
        last = None
        for ri, (p0, _) in enumerate(self.rows):
            for r in regs:
                if re.match(r"(STR|STLR|SWP\w*)\s+.*\[%s[\],]" % r, p0):
                    last = (ri, p0)
        return last

    def render(self, rows):
        w0 = max(len(r[0]) for r in rows + [["P0", ""]]) + 2
        out = self.lines[:self.body_start - 1]
        out.append(" P0" + " " * (w0 - 2) + "| P1 ;")
        for p0, p1 in rows:
            out.append(" " + p0 + " " * (w0 - len(p0)) + "| " + p1 + " ;")
        out.append(self.exists)
        return "\n".join(out) + "\n"


def candidates(lit):
    """Yield (name, description, rows) fixed variants."""
    var, p1row, p1mn = lit.flag_var()
    if var is None:
        return
    st = lit.p0_store_row(var)
    if st is None:
        return
    p0row, p0ins = st
    p1ins = lit.rows[p1row][1]

    def rows_copy():
        return [list(r) for r in lit.rows]

    # 1. promote
    r = rows_copy()
    changed = False
    if p0ins.startswith("STR "):
        r[p0row][0] = "STLR" + p0ins[3:]; changed = True
    if p1ins.startswith("LDR "):
        r[p1row][1] = "LDAR" + p1ins[3:]; changed = True
    if changed:
        yield ("promote", f"P0 {lit.anchor_fn}: store to '{var}' -> smp_store_release; "
                          f"P1 {lit.partner_fn}: load of '{var}' -> smp_load_acquire", r)

    # 2. wmb / rmb
    def insert(rows, tid, at, ins, before):
        rows = [list(x) for x in rows]
        new = ["", ""]; new[tid] = ins
        rows.insert(at if before else at + 1, new)
        # keep the other column aligned: nothing to do, empty cell is fine
        return rows
    r = rows_copy()
    r = insert(r, 0, p0row, "DMB ST", before=True)
    r = insert(r, 1, p1row + (1 if p1row >= p0row else 0), "DMB LD", before=False)
    yield ("wmb-rmb", f"P0 {lit.anchor_fn}: smp_wmb() before store to '{var}'; "
                      f"P1 {lit.partner_fn}: smp_rmb() after load of '{var}'", r)

    # 3. mb / mb
    r = rows_copy()
    r = insert(r, 0, p0row, "DMB ISH", before=True)
    r = insert(r, 1, p1row + (1 if p1row >= p0row else 0), "DMB ISH", before=False)
    yield ("mb-mb", f"P0 {lit.anchor_fn}: smp_mb() before store to '{var}'; "
                    f"P1 {lit.partner_fn}: smp_mb() after load of '{var}'", r)

    # 4. promote + mb
    r = rows_copy()
    if p0ins.startswith("STR "):
        r[p0row][0] = "STLR" + p0ins[3:]
    if p1ins.startswith("LDR "):
        r[p1row][1] = "LDAR" + p1ins[3:]
    r = insert(r, 0, p0row, "DMB ISH", before=True)
    r = insert(r, 1, p1row + (1 if p1row >= p0row else 0), "DMB ISH", before=False)
    yield ("promote+mb", f"release/acquire on '{var}' plus smp_mb() on both sides", r)


def dbg_locations(ll_path):
    """Map '!N' -> 'file:line' from a module .ll with line tables."""
    if not ll_path or not Path(ll_path).exists():
        return {}, {}
    text = Path(ll_path).read_text(errors="replace")
    files = {}
    for m in re.finditer(r"^(!\d+) = !DIFile\(filename: \"([^\"]+)\"", text, re.M):
        files[m.group(1)] = m.group(2)
    scopes = {}
    for m in re.finditer(r"^(!\d+) = (?:distinct )?!DI(?:Subprogram|LexicalBlock)\(.*?file: (!\d+)", text, re.M):
        scopes[m.group(1)] = m.group(2)
    locs = {}
    for m in re.finditer(r"^(!\d+) = !DILocation\(line: (\d+),(?: column: \d+,)? scope: (!\d+)", text, re.M):
        fid = scopes.get(m.group(3))
        locs[m.group(1)] = f"{files.get(fid, '?')}:{m.group(2)}"
    return locs, files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("litmus_dir")
    ap.add_argument("--ll", help="module .ll (with -g line tables) for source mapping")
    ap.add_argument("--herd7", default=HERD7)
    ap.add_argument("--herdlib", default=HERDLIB)
    ap.add_argument("--retries", type=int, default=4, help="max candidate fixes tried per test")
    ap.add_argument("--only-no", action="store_true", help="skip tests that are already Ok")
    ap.add_argument("--json", type=Path, default=None,
                    help="also write the result of each synthesis as JSON")
    args = ap.parse_args()

    locs, _ = dbg_locations(args.ll)
    total = fixed = 0
    records = []
    print(f"{'litmus':60s} {'verdict':7s} {'fix':11s} {'after':6s}")
    for f in sorted(Path(args.litmus_dir).glob("*.litmus")):
        if f.name.endswith(".fixed.litmus"):
            continue
        v, _ = run_herd7(f, args.herd7, args.herdlib)
        if v != "No":
            if not args.only_no:
                print(f"{f.stem:60s} {v:7s}")
            continue
        total += 1
        lit = Litmus(f.read_text())
        found = None
        tried = 0
        for name, desc, rows in candidates(lit):
            if tried >= args.retries:
                break
            tried += 1
            fixed_path = f.with_suffix("").with_suffix(f".{name}.fixed.litmus")
            fixed_path.write_text(lit.render(rows))
            fv, fobs = run_herd7(fixed_path, args.herd7, args.herdlib)
            if fv == "Ok":
                found = (name, desc, fobs, fixed_path)
                break
        report = [f"litmus   : {f.name}",
                  f"anchor   : {lit.anchor_desc}",
                  f"functions: P0={lit.anchor_fn}  P1={lit.partner_fn}",
                  f"verdict  : No (forbidden state reachable)"]
        for h in lit.header:
            tm = re.match(r"\(\* (P[01]\.\S+(?: \S+)?): (.*) \*\)$", h)
            if not tm:
                continue
            tag, ir = tm.group(1), tm.group(2)
            dm = re.search(r"!dbg (!\d+)", ir)
            loc = locs.get(dm.group(1), "") if dm else ""
            short = re.sub(r", !dbg !\d+$", "", ir)[:90]
            report.append(f"{tag:14s}: {short}" + (f"   @ {loc}" if loc else ""))
        if found:
            fixed += 1
            report += [f"fix      : {found[0]}  ({tried} candidate(s) tried)",
                       f"patch    : {found[1]}",
                       f"verified : herd7 {found[2]}  [{found[3].name}]"]
        else:
            report += [f"fix      : none of {tried} candidates makes the forbidden state unreachable",
                       "note     : the violation is not a two-sided barrier/acquire-release defect on the flag variable"]
        f.with_suffix(".patch.txt").write_text("\n".join(report) + "\n")
        records.append({
            "litmus":     f.name,
            "anchor_fn":  lit.anchor_fn,
            "partner_fn": lit.partner_fn,
            "anchor":     lit.anchor_desc,
            "fix":        found[0] if found else None,
            "verified":   bool(found),
            "candidates_tried": tried,
        })
        print(f"{f.stem:60s} {'No':7s} {(found[0] if found else '-'):11s} {('Ok' if found else '-'):6s}")
    print(f"\n{total} No test(s), {fixed} patched and verified, {total - fixed} unpatched")

    if args.json:
        by_fix = {}
        for r in records:
            by_fix[r["fix"] or "none"] = by_fix.get(r["fix"] or "none", 0) + 1
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "violations": total,
            "patched":    fixed,
            "unpatched":  total - fixed,
            "by_fix":     by_fix,
            "records":    records,
        }, indent=2) + "\n")


if __name__ == "__main__":
    main()
