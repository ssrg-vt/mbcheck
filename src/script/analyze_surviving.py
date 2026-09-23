#!/usr/bin/env python3
"""
analyze_surviving.py — parse kernel_noLTO_results.log and characterise
the surviving fn-pair population to identify missing heuristics.

Usage:
    python3 analyze_surviving.py [--log PATH] [--top N]
"""
import argparse, re, sys, collections
from dataclasses import dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------
# Log block per pair (inside PASS: PAIRS SURVIVING):
#
#   Fn-pairs: {anchor_fn, partner_fn }
#   anchor_fn:
#     Anchor: [obj]  label  (site)
#     Before:  [obj]  IR
#     After:   [obj]  IR
#
#   partner_fn
#     Accesses:
#       [obj]  IR

@dataclass
class PairRecord:
    anchor_fn: str = ""
    partner_fn: str = ""
    anchor_obj: str = ""      # object tag from Anchor: line
    before_ir: Optional[str] = None
    after_ir: Optional[str] = None
    before_obj: Optional[str] = None
    after_obj: Optional[str] = None
    partner_accesses: list = field(default_factory=list)  # list of (obj, ir)
    # derived
    anchor_label: str = ""
    file: str = ""

    # ---- classification helpers ----
    def anchor_has_store(self):
        for ir in [self.before_ir, self.after_ir]:
            if ir and _is_store(ir):
                return True
        return False

    def partner_has_store(self):
        for _, ir in self.partner_accesses:
            if _is_store(ir):
                return True
        return False

    def is_read_read(self):
        return not self.anchor_has_store() and not self.partner_has_store()

    def anchor_object_names(self):
        objs = set()
        for obj in [self.before_obj, self.after_obj]:
            if obj:
                objs.add(obj)
        return objs

    def partner_object_names(self):
        return {obj for obj, _ in self.partner_accesses}

    def shared_objects(self):
        return self.anchor_object_names() & self.partner_object_names()

    def anchor_accesses_atomic(self):
        """Both bracket accesses are atomic IR (ldar/stlr/ldadd etc.)"""
        for ir in [self.before_ir, self.after_ir]:
            if ir and not _is_atomic_ir(ir):
                return False
        return bool(self.before_ir or self.after_ir)

    def partner_accesses_atomic(self):
        if not self.partner_accesses:
            return False
        return all(_is_atomic_ir(ir) for _, ir in self.partner_accesses)


def _is_store(ir: str) -> bool:
    s = ir.strip()
    return (s.startswith("store ") or
            " store " in s or
            "stlr" in s or "stl " in s or
            "swpal" in s or
            ("ldadd" in s and "al" in s))  # ldaddal is RMW → has write

def _is_atomic_ir(ir: str) -> bool:
    keywords = ("ldar", "stlr", "ldadd", "ldset", "ldclr", "ldeor",
                "casal", "swpal", "cas ", "dmb ", "dsb ")
    s = ir.lower()
    return any(k in s for k in keywords)


# ---------------------------------------------------------------------------
# Parse
# ---------------------------------------------------------------------------
def parse_log(path: str):
    records = []
    cur_file = ""
    in_surviving = False
    cur: Optional[PairRecord] = None
    in_anchor_block = False
    in_partner_block = False

    with open(path) as fh:
        lines = fh.readlines()

    for raw in lines:
        raw = raw.rstrip('\n')
        stripped = raw.strip()

        # ── file boundary ────────────────────────────────────────────────
        if raw.startswith("FILE :"):
            if cur:
                records.append(cur)
                cur = None
            cur_file = raw[7:].strip()
            in_surviving = False
            in_anchor_block = False
            in_partner_block = False
            continue

        # ── section transitions (never flip on '====' separator lines) ──
        if "PASS: PAIRS SURVIVING" in raw:
            if cur:
                records.append(cur)
                cur = None
            in_surviving = True
            in_anchor_block = False
            in_partner_block = False
            continue
        if "PASS: PAIRS FILTERED" in raw and in_surviving:
            if cur:
                records.append(cur)
                cur = None
            in_surviving = False
            continue

        # ignore '====' dividers and PASS headers for other sections
        if stripped.startswith("====") or (
                raw.startswith("PASS:") and "SURVIVING" not in raw):
            continue

        if not in_surviving:
            continue

        # ── new pair ─────────────────────────────────────────────────────
        if raw.startswith("Fn-pairs:"):
            if cur:
                records.append(cur)
            m = re.match(r"Fn-pairs: \{(.+?), (.+?) \}", raw)
            if m:
                cur = PairRecord(file=cur_file,
                                 anchor_fn=m.group(1).strip(),
                                 partner_fn=m.group(2).strip())
                in_anchor_block = True
                in_partner_block = False
            else:
                cur = None
            continue

        if cur is None:
            continue

        # ── block transitions inside a pair ──────────────────────────────
        # "anchor_fn:" header
        if raw == cur.anchor_fn + ":":
            in_anchor_block = True
            in_partner_block = False
            continue
        # "partner_fn" header (no colon)
        if raw == cur.partner_fn:
            in_anchor_block = False
            in_partner_block = False
            continue
        if stripped == "Accesses:":
            in_partner_block = True
            continue

        # ── anchor block content ─────────────────────────────────────────
        if in_anchor_block:
            m = re.match(r"\s+Anchor:\s+\[(.+?)\]\s+(\S+)", raw)
            if m:
                cur.anchor_obj   = m.group(1)
                cur.anchor_label = m.group(2)
                continue
            m = re.match(r"\s+Before:\s+\[(.+?)\]\s+(.+)", raw)
            if m:
                cur.before_obj = m.group(1)
                cur.before_ir  = m.group(2)
                continue
            m = re.match(r"\s+After:\s+\[(.+?)\]\s+(.+)", raw)
            if m:
                cur.after_obj = m.group(1)
                cur.after_ir  = m.group(2)
                continue

        # ── partner accesses block ────────────────────────────────────────
        elif in_partner_block:
            m = re.match(r"\s+\[(.+?)\]\s+(.+)", raw)
            if m:
                cur.partner_accesses.append((m.group(1), m.group(2)))

    if cur:
        records.append(cur)
    return records


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------
def analyze(records, top_n=20):
    total = len(records)
    print(f"Total surviving pair records parsed: {total:,}\n")

    # 1. Read-read
    rr = [r for r in records if r.is_read_read()]
    print(f"[H_RR] Read-Read pairs (no store on either side): {len(rr):,}  ({100*len(rr)/total:.1f}%)")

    # 2. Both sides atomic
    aa = [r for r in records if r.anchor_accesses_atomic() and r.partner_accesses_atomic()]
    print(f"[H_AA] Both sides atomic-only accesses:           {len(aa):,}  ({100*len(aa)/total:.1f}%)")

    # 3. Partner has no accesses (empty partner_accesses)
    empty_partner = [r for r in records if not r.partner_accesses]
    print(f"[H_EP] Partner has no accesses recorded:          {len(empty_partner):,}  ({100*len(empty_partner)/total:.1f}%)")

    # 4. Same anchor+partner repeated (duplicate pairs from multiple brackets)
    pair_counts = collections.Counter((r.file, r.anchor_fn, r.partner_fn) for r in records)
    dup_pairs = {k: v for k, v in pair_counts.items() if v > 1}
    dup_total = sum(v - 1 for v in dup_pairs.values())
    print(f"[H_DUP] Duplicate (fn1,fn2) in same file (extra): {dup_total:,}  ({100*dup_total/total:.1f}%)")

    # 5. Shared object is a refcount-like global (name heuristic)
    REFCOUNT_PATS = re.compile(r'refcount|kref|atomic_t|ref_cnt|use_count|users|nref|nr_users', re.I)
    refcount = [r for r in records if any(REFCOUNT_PATS.search(o) for o in r.shared_objects())]
    print(f"[H_REF] Shared obj looks like a refcount:         {len(refcount):,}  ({100*len(refcount)/total:.1f}%)")

    # 6. Shared object is a stats/counter-like global (perf counter, not a race)
    STATS_PATS = re.compile(r'stat|counter|count|_nr_|_num_|bytes|packets|dropped|errors|tx_|rx_', re.I)
    stats_pairs = [r for r in records if all(STATS_PATS.search(o) for o in (r.shared_objects() or [""]))]
    print(f"[H_STAT] Shared obj looks like a perf counter:    {len(stats_pairs):,}  ({100*len(stats_pairs)/total:.1f}%)")

    # 7. Anchor object is a per-cpu variable
    PERCPU_PATS = re.compile(r'percpu|per_cpu|__percpu|this_cpu', re.I)
    percpu = [r for r in records if PERCPU_PATS.search(r.anchor_obj)]
    print(f"[H_CPU] Anchor obj looks per-cpu:                 {len(percpu):,}  ({100*len(percpu)/total:.1f}%)")

    print()

    # Most impactful single new heuristic: read-read
    print("=" * 70)
    print("TOP CANDIDATE: H_RR  (Read-Read filter)")
    print("=" * 70)
    print(f"  Would filter {len(rr):,} more pairs ({100*len(rr)/total:.1f}% of surviving)")
    print()

    # After removing rr, what remains?
    remaining = [r for r in records if not r.is_read_read()]
    print(f"  After H_RR: {len(remaining):,} pairs remain")
    if remaining:
        rr2 = [r for r in remaining if r.anchor_accesses_atomic() and r.partner_accesses_atomic()]
        print(f"  Of those, {len(rr2):,} are both-atomic ({100*len(rr2)/len(remaining):.1f}%)")

    print()
    print("Top files by read-read surviving pairs:")
    rr_per_file = collections.Counter(r.file for r in rr)
    for fn, cnt in rr_per_file.most_common(15):
        print(f"  {cnt:>8,}  {fn}")

    print()
    print("Top shared-object names in read-read pairs (sample):")
    obj_ctr = collections.Counter()
    for r in rr:
        for o in r.shared_objects():
            obj_ctr[o] += 1
    for obj, cnt in obj_ctr.most_common(20):
        print(f"  {cnt:>8,}  {obj}")

    print()
    print("=" * 70)
    print("ANCHOR LABELS in surviving pairs (all, not just rr):")
    print("=" * 70)
    label_ctr = collections.Counter(r.anchor_label for r in records)
    for lab, cnt in label_ctr.most_common(20):
        rr_cnt = sum(1 for r in records if r.anchor_label == lab and r.is_read_read())
        print(f"  {cnt:>8,}  {lab:<30}  (rr={rr_cnt:,})")

    print()
    print("=" * 70)
    print("TOP 20 anchor-function names driving surviving pairs:")
    print("=" * 70)
    fn_ctr = collections.Counter(r.anchor_fn for r in records)
    for fn, cnt in fn_ctr.most_common(20):
        rr_cnt = sum(1 for r in records if r.anchor_fn == fn and r.is_read_read())
        print(f"  {cnt:>8,}  {fn:<50}  rr={rr_cnt:,}")

    print()
    print("=" * 70)
    print("SAMPLE non-rr surviving pairs (first 10 with a store):")
    print("=" * 70)
    shown = 0
    for r in records:
        if not r.is_read_read() and shown < 10:
            print(f"  File: {r.file}")
            print(f"  Pair: {r.anchor_fn} x {r.partner_fn}")
            print(f"  Anchor obj: {r.anchor_obj}  label: {r.anchor_label}")
            print(f"  Before: {r.before_ir}")
            print(f"  After:  {r.after_ir}")
            for obj, ir in r.partner_accesses[:3]:
                print(f"    Partner [{obj}]: {ir[:100]}")
            print()
            shown += 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True,
                    help="sweep log from run_kernel_noLTO.py")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()
    recs = parse_log(args.log)
    analyze(recs, args.top)
