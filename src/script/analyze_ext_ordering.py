#!/usr/bin/env python3
"""analyze_ext_ordering.py — Summarise the memory-ordering effect of external
functions called by each kernel module in tests/kernel-noLTO.

For every .ll file the script:
  1. Collects all declared (external) function names.
  2. For each defined function body, scans:
       a. Inline-asm strings for known AArch64 ordering instructions.
       b. Volatile load/store instructions (READ_ONCE / WRITE_ONCE).
       c. Calls to declared externals whose name matches a CSV anchor entry.
  3. Attributes the strongest ordering found to each (module, external-fn) pair.
  4. Writes docs/ext-fn-ordering.csv with columns:
       module, external_function, asm_instruction, ordering

Ordering strength ranking (strongest first):
  SC > acquire > release > relaxed > compiler-only > (none)

Usage
-----
  python3 src/script/analyze_ext_ordering.py [OPTIONS]

Options
-------
  -i <dir>   Input directory of .ll files   (default: tests/kernel-noLTO)
  -c <csv>   Anchor-API CSV                 (default: docs/kernel-nb-anchor-apis-grouped.csv)
  -o <csv>   Output CSV                     (default: docs/ext-fn-ordering.csv)
  -j <n>     Parallel jobs                  (default: cpu count)
  -v         Verbose
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

# ---------------------------------------------------------------------------
# Ordering strength (higher index = stronger)
# ---------------------------------------------------------------------------
ORDERING_RANK = {
    "compiler-only": 1,
    "relaxed":       2,
    "release":       3,
    "acquire":       4,
    "SC":            5,
}

def strongest(a, b):
    """Return whichever ordering string is stronger."""
    if a is None:
        return b
    if b is None:
        return a
    return a if ORDERING_RANK.get(a, 0) >= ORDERING_RANK.get(b, 0) else b


# ---------------------------------------------------------------------------
# CSV loading
# ---------------------------------------------------------------------------
def load_anchor_csv(csv_path):
    """
    Returns two structures:

    asm_map: {asm_key: (asm_label, ordering)}
        asm_key  = canonical asm mnemonic, lower-cased, spaces collapsed
                   e.g. "dmb ish", "ldaddal", "casal"
        asm_label = original string from CSV column 1
        ordering  = "SC" | "acquire" | "release" | "relaxed" | "compiler-only"

    ext_map: {ext_key: (asm_label, ordering)}
        ext_key  = lower-cased name fragment from "(ext)" rows, without "(ext)"
                   e.g. "_raw_spin_lock", "call_rcu", "wake_up"
        Matching: a declared function name matches if it starts with ext_key.

    volatile_load_entry  : (asm_label, ordering) for "load volatile ptr"
    volatile_store_entry : (asm_label, ordering) for "store volatile ptr"
    """
    asm_map   = {}   # asm_key  -> (label, ordering)
    ext_map   = {}   # ext_key  -> (label, ordering)
    vol_load  = None
    vol_store = None

    with open(csv_path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            label    = row["asm_instruction"].strip()
            ordering_raw = row["ordering"].strip()
            # Some cells have "acquire | compiler-only" because they cover
            # two different APIs with different guarantees (e.g. READ_ONCE vs
            # rcu_dereference for "load volatile ptr").  We cannot distinguish
            # them in IR, so take the *weakest* ordering to avoid overstating
            # the guarantee.
            orderings = [o.strip() for o in ordering_raw.split("|")]
            ordering  = orderings[0]
            for o in orderings[1:]:
                # keep the weaker one
                if ORDERING_RANK.get(o, 0) < ORDERING_RANK.get(ordering, 0):
                    ordering = o

            if label == "load volatile ptr":
                vol_load = (label, ordering)
            elif label == "store volatile ptr":
                vol_store = (label, ordering)
            elif label.endswith("(ext)"):
                # Strip " (ext)" suffix to get the match key
                ext_key = label[:-len(" (ext)")].strip().lower()
                ext_map[ext_key] = (label, ordering)
            else:
                # Inline asm mnemonic row; normalise whitespace
                asm_key = " ".join(label.lower().split())
                asm_map[asm_key] = (label, ordering)

    return asm_map, ext_map, vol_load, vol_store


# ---------------------------------------------------------------------------
# IR text parsing helpers
# ---------------------------------------------------------------------------
# Matches: asm sideeffect "...", ...
_ASM_RE = re.compile(r'asm\s+sideeffect\s+"([^"]*)"')

# Matches: load volatile or store volatile
_VLOAD_RE  = re.compile(r'\bload\s+volatile\b')
_VSTORE_RE = re.compile(r'\bstore\s+volatile\b')

# Matches:  call ... @name(
_CALL_RE = re.compile(r'\bcall\b[^@]*@([A-Za-z_][A-Za-z0-9_.]*)\s*\(')

# Matches: declare ...  @name(
_DECL_RE = re.compile(r'^declare\b.*@([A-Za-z_][A-Za-z0-9_.]*)\s*\(', re.MULTILINE)

# define line
_DEFINE_RE = re.compile(r'^define\b', re.MULTILINE)


def extract_asm_mnemonics(asm_str):
    """
    Given the raw asm string (with \09 = TAB, \0A = newline escape sequences),
    return the set of normalised mnemonic tokens found.
    Multi-word mnemonics like "dmb ish" or "dmb ishst" need to be matched as
    a prefix of instruction lines.
    """
    # Unescape common escape sequences
    text = asm_str.replace("\\09", "\t").replace("\\0A", "\n")
    mnemonics = set()
    for line in text.split("\n"):
        line = line.strip().lstrip("\t ")
        if not line or line.startswith("//") or line.startswith(".") or line.startswith("%"):
            continue
        # Normalise: collapse whitespace, drop operands
        parts = line.split()
        if not parts:
            continue
        # Try two-word mnemonic first (e.g. "dmb ish"), then one-word
        if len(parts) >= 2:
            mnemonics.add((parts[0] + " " + parts[1]).lower())
        mnemonics.add(parts[0].lower())
    return mnemonics


def match_asm(mnemonics, asm_map):
    """
    Find the strongest ordering in asm_map that matches any mnemonic.
    Returns (label, ordering) or None.
    """
    best_label    = None
    best_ordering = None
    for m in mnemonics:
        if m in asm_map:
            label, ordering = asm_map[m]
            best_ordering = strongest(best_ordering, ordering)
            if best_ordering == ordering:
                best_label = label
    return (best_label, best_ordering) if best_ordering else None


def match_ext(fn_name, ext_map):
    """
    Match a declared function name against ext_map by prefix.
    Returns (label, ordering) or None.
    """
    fn_lower = fn_name.lower()
    best = None
    best_len = 0
    for ext_key, entry in ext_map.items():
        if fn_lower.startswith(ext_key) and len(ext_key) > best_len:
            best = entry
            best_len = len(ext_key)
    return best


# ---------------------------------------------------------------------------
# Per-file analysis
# ---------------------------------------------------------------------------
def analyse_ll(ll_path, asm_map, ext_map, vol_load, vol_store, module_root):
    """
    Parse one .ll file.

    Returns a list of:
      (module_name, ext_fn_name, asm_label, ordering)

    where each row represents a declared external function that either:
      - matches an anchor ext_map entry directly, OR
      - is the sole "target" of an inline-asm ordering instruction found in a
        function body that calls it (we attribute the asm to the function).

    Design:
      We scan every *defined* function body.  For each body we collect:
        • inline asm orderings  (attributed as "(inline asm in caller)")
        • volatile load/store   (ditto)
        • direct calls to declared externals  -> match ext_map by name

      Each declared external function called anywhere in the module gets the
      strongest ordering it transitively implies.
    """
    try:
        text = ll_path.read_text(errors="replace")
    except OSError:
        return []

    # Relative module name: path under module_root, stem only
    try:
        rel = ll_path.relative_to(module_root)
    except ValueError:
        rel = Path(ll_path.name)
    module_name = str(rel.with_suffix(""))   # e.g. "mm/memfd"

    # 1. Collect declared external function names
    declared = set(m.group(1) for m in _DECL_RE.finditer(text)
                   if not m.group(1).startswith("llvm."))

    # 2. Split text into per-function bodies
    #    Each "define ..." block ends at the next top-level "}" line.
    fn_bodies = []
    for m in _DEFINE_RE.finditer(text):
        start = m.start()
        # Find matching closing brace at top of line
        pos = start
        depth = 0
        while pos < len(text):
            ch = text[pos]
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0:
                    fn_bodies.append(text[start:pos+1])
                    break
            pos += 1

    # 3. For each declared external, determine its ordering.
    #    Precedence: direct ext_map name match > caller-context asm.
    #    We collect per-declared-fn the strongest ordering found across all callers.
    ext_ordering = {}   # fn_name -> (label, ordering)

    for fn_name in declared:
        entry = match_ext(fn_name, ext_map)
        if entry:
            label, ordering = entry
            cur = ext_ordering.get(fn_name)
            if cur is None or ORDERING_RANK.get(ordering, 0) > ORDERING_RANK.get(cur[1], 0):
                ext_ordering[fn_name] = (label, ordering)

    # 4. Scan each function body for asm/volatile/calls
    #    Attribute inline-asm orderings to each external called in the same body.
    for body in fn_bodies:
        # Collect inline asm orderings in this body
        body_asm_ordering = None
        for asm_m in _ASM_RE.finditer(body):
            mnemonics = extract_asm_mnemonics(asm_m.group(1))
            result = match_asm(mnemonics, asm_map)
            if result:
                _, ordering = result
                if body_asm_ordering is None:
                    body_asm_ordering = result
                else:
                    body_asm_ordering = (
                        result[0] if ORDERING_RANK.get(ordering, 0) > ORDERING_RANK.get(body_asm_ordering[1], 0) else body_asm_ordering[0],
                        strongest(body_asm_ordering[1], ordering)
                    )

        # volatile loads/stores in this body
        if _VLOAD_RE.search(body) and vol_load:
            label, ordering = vol_load
            if body_asm_ordering is None or ORDERING_RANK.get(ordering, 0) > ORDERING_RANK.get(body_asm_ordering[1], 0):
                body_asm_ordering = (label, ordering)

        if _VSTORE_RE.search(body) and vol_store:
            label, ordering = vol_store
            if body_asm_ordering is None or ORDERING_RANK.get(ordering, 0) > ORDERING_RANK.get(body_asm_ordering[1], 0):
                body_asm_ordering = (label, ordering)

        # Calls to declared externals in this body
        for call_m in _CALL_RE.finditer(body):
            fn_name = call_m.group(1)
            if fn_name not in declared:
                continue
            # If not yet matched by name, try to inherit body asm ordering
            if fn_name not in ext_ordering and body_asm_ordering:
                ext_ordering[fn_name] = body_asm_ordering
            elif fn_name in ext_ordering and body_asm_ordering:
                cur_label, cur_ord = ext_ordering[fn_name]
                new_label, new_ord = body_asm_ordering
                if ORDERING_RANK.get(new_ord, 0) > ORDERING_RANK.get(cur_ord, 0):
                    ext_ordering[fn_name] = (new_label, new_ord)

    # 5. Build result rows — only include externals for which we found an ordering
    rows = []
    for fn_name, (label, ordering) in sorted(ext_ordering.items()):
        rows.append((module_name, fn_name, label, ordering))

    return rows


def _analyse_ll_worker(ll_path, args_tuple):
    """Module-level picklable wrapper for ProcessPoolExecutor."""
    asm_map, ext_map, vol_load, vol_store, input_dir_str = args_tuple
    return analyse_ll(ll_path, asm_map, ext_map, vol_load, vol_store, Path(input_dir_str))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    script_dir  = Path(__file__).resolve().parent
    project_dir = script_dir.parent.parent  # <project>/src/script -> <project>

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input",   default=str(project_dir / "tests" / "kernel-noLTO"),
                    help="Directory of .ll files")
    ap.add_argument("-c", "--csv",     default=str(project_dir / "docs" / "kernel-nb-anchor-apis-grouped.csv"),
                    help="Anchor-API CSV")
    ap.add_argument("-o", "--output",  default=str(project_dir / "docs" / "ext-fn-ordering.csv"),
                    help="Output CSV path")
    ap.add_argument("-j", "--jobs",    type=int, default=os.cpu_count(),
                    help="Parallel jobs")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    input_dir   = Path(args.input)
    anchor_csv  = Path(args.csv)
    output_csv  = Path(args.output)

    if not input_dir.is_dir():
        sys.exit(f"Input directory not found: {input_dir}")
    if not anchor_csv.is_file():
        sys.exit(f"Anchor CSV not found: {anchor_csv}")

    print(f"Input  : {input_dir}")
    print(f"Anchors: {anchor_csv}")
    print(f"Output : {output_csv}")

    asm_map, ext_map, vol_load, vol_store = load_anchor_csv(anchor_csv)
    if args.verbose:
        print(f"  Loaded {len(asm_map)} asm entries, {len(ext_map)} ext entries from CSV")

    ll_files = sorted(input_dir.rglob("*.ll"))
    print(f"Processing {len(ll_files)} .ll files with {args.jobs} workers...")

    all_rows = []

    # Build a picklable args tuple for the worker
    _worker_args = (asm_map, ext_map, vol_load, vol_store, str(input_dir))

    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        futures = {ex.submit(_analyse_ll_worker, p, _worker_args): p for p in ll_files}
        done = 0
        for fut in as_completed(futures):
            done += 1
            rows = fut.result()
            all_rows.extend(rows)
            if args.verbose and done % 200 == 0:
                print(f"  {done}/{len(ll_files)} done, {len(all_rows)} rows so far")

    # Sort: module, then external function name
    all_rows.sort(key=lambda r: (r[0], r[1]))

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(output_csv, "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["module", "external_function", "asm_instruction", "ordering"])
        writer.writerows(all_rows)

    print(f"Done. Wrote {len(all_rows)} rows to {output_csv}")

    # Summary by ordering tier
    from collections import Counter
    counts = Counter(r[3] for r in all_rows)
    for tier in ["SC", "acquire", "release", "relaxed", "compiler-only"]:
        if counts[tier]:
            print(f"  {tier:15s}: {counts[tier]} entries")


if __name__ == "__main__":
    main()
