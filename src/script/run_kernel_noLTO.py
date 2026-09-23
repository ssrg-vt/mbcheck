#!/usr/bin/env python3
"""
run_kernel_noLTO.py  -  Run mbcheck --summary on every .ll file under
tests/kernel-noLTO.  Each IR module is analysed independently in a separate
mbcheck invocation.

For each module the script captures the machine-parseable STATS: lines emitted
by --summary and aggregates them into a combined report.

Outputs
-------
  - Rolling progress to stdout (one line per file)
  - A detailed per-file log   (default: build/kernel_noLTO_results.log)
  - A full summary report     (appended to the log and printed to stdout)

Summary sections
----------------
  1. IR files (modules) scanned
  2. Anchors found -- total and per-label breakdown with counts + %
  3. Causal allocations (brackets) identified
  4. Fn-pairs detected (raw, before filtering)
  5. Fn-pairs filtered -- per-heuristic breakdown (most to least) with %
  6. Fn-pairs NOT filtered (surviving / reported)
  7. Herd7 litmus files generated
  8. Herd7 simulator validation results (AArch64 model)

Usage
-----
  python3 src/script/run_kernel_noLTO.py [OPTIONS]

Options
-------
  --mbcheck PATH   Path to mbcheck binary          (default: <project>/build/mbcheck)
  --testdir DIR    Root of kernel-noLTO IR tree    (default: <project>/tests/kernel-noLTO)
  --jobs N         Parallel worker count           (default: nproc)
  --log FILE       Detailed log output path        (default: <project>/build/kernel_noLTO_results.log)
  --limit N         Process only the first N files  (useful for quick tests)
  --limit-herd7 N   Max total herd7 invocations     (default: unlimited, 0 = unlimited)
  --herd7 PATH      Path to herd7 binary            (default: auto-detected)
  --litmusdir DIR   Directory to write .litmus files (default: <log_dir>/litmus)
  --no-herd7        Skip herd7 validation entirely
"""

import argparse
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Path defaults
# ---------------------------------------------------------------------------
_SCRIPT_DIR   = Path(__file__).resolve().parent   # src/script/
_PROJECT_ROOT = _SCRIPT_DIR.parent.parent          # project root

DEFAULT_MBCHECK  = _PROJECT_ROOT / "build" / "mbcheck"
DEFAULT_TESTDIR  = _PROJECT_ROOT / "tests" / "kernel-noLTO-v6.19"
DEFAULT_LOG      = _PROJECT_ROOT / "build" / "kernel_noLTO_results.log"

# herd7 .cat directory, resolved in main().  A source build cannot find
# stdlib.cat on its own.
_HERDLIB = None


def _resolve_herd7(explicit):
    """herd7 binary: --herd7, else $HERD7, else the first one on $PATH."""
    if explicit is not None:
        return Path(explicit)
    env = os.environ.get("HERD7")
    if env:
        return Path(env)
    for name in ("herd7", "herd.exe"):
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def _resolve_herdlib(herd7_bin):
    """$HERDLIB, else derived from the binary location."""
    env = os.environ.get("HERDLIB")
    if env:
        return env
    if herd7_bin is None:
        return None
    for parent in herd7_bin.resolve().parents:
        for rel in ("herd/libdir", "share/herdtools7/herd"):
            cand = parent / rel
            if (cand / "stdlib.cat").is_file():
                return str(cand)
    return None


_PROBE_LITMUS = """AArch64 mbcheck-probe
{
0:X0=x; 1:X0=x;
}
 P0           | P1            ;
 MOV W1,#1    | LDR W1,[X0]   ;
 STR W1,[X0]  |               ;
exists (1:X1=1)
"""


def _check_herd7(herd7_bin):
    """Probe herd7 so a broken setup fails now, not after the sweep."""
    with tempfile.TemporaryDirectory() as td:
        probe = Path(td) / "probe.litmus"
        probe.write_text(_PROBE_LITMUS)
        argv = [str(herd7_bin)]
        if _HERDLIB:
            argv += ["-set-libdir", _HERDLIB]
        argv.append(str(probe))
        try:
            proc = subprocess.run(argv, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True,
                                  timeout=60)
        except Exception as exc:
            return f"could not execute {herd7_bin}: {exc}"
    if not any(line.strip() in ("Ok", "No") for line in proc.stdout.splitlines()):
        detail = proc.stdout.strip().splitlines()
        detail = detail[0] if detail else f"exit {proc.returncode}, no output"
        hint = ""
        if "stdlib.cat" in proc.stdout:
            hint = ("\n  herd7 cannot find its .cat files.  Set $HERDLIB to the "
                    "directory containing stdlib.cat (a source build keeps it "
                    "at <herdtools7>/herd/libdir).")
        return f"herd7 did not answer on a trivial litmus test: {detail}{hint}"
    return None

_RE_ANCHORS_TOTAL  = re.compile(r"^STATS:anchors:total=(\d+)$")
_RE_ANCHOR_LABEL   = re.compile(r"^STATS:anchor_label:(.+)=(\d+)$")
_RE_CAUSAL_TOTAL   = re.compile(r"^STATS:causal:total=(\d+)$")
_RE_PAIRS_TOTAL    = re.compile(r"^STATS:pairs:total=(\d+)$")
_RE_PAIRS_FILTERED = re.compile(r"^STATS:pairs:filtered=(\d+)$")
_RE_PAIRS_SURVIVE  = re.compile(r"^STATS:pairs:surviving=(\d+)$")
_RE_HEURISTIC      = re.compile(r"^STATS:heuristic:(.+)=(\d+)$")
_RE_HEURISTIC_SUB  = re.compile(r"^STATS:heuristic_sub:([^:]+):(.+)=(\d+)$")
_RE_LITMUS_TOTAL   = re.compile(r"^STATS:litmus:total=(\d+)$")
_RE_PHASE_NS       = re.compile(r"^STATS:time:([a-z_]+)=(\d+)$")


# Extra args forwarded to every mbcheck invocation (e.g. --disable-heuristic=...).
_MBCHECK_EXTRA: list[str] = []


# ---------------------------------------------------------------------------
# v14 path-1 — Cross-TU __initcall reachability pre-pass
# ---------------------------------------------------------------------------
# H27 (LifecycleHeuristic) only sees the call graph of one IR module.  Many
# kernel "init-time producer" functions (memblock_add, pptt_parse_*, etc.)
# are exported with hidden linkage so their *in-module* callers all look like
# leaf functions — H27's internal-linkage gate then refuses to mark them
# init-only and the pair survives, becoming a No verdict in herd7.
#
# This helper performs a corpus-wide pre-pass:
#   1. Scan every .ll file for `__initcall_…` / `__exitcall_…` markers and
#      defined functions.
#   2. Build a global call graph (caller → callees) by parsing every
#      `@<name>(` reference inside `define { … }` bodies.
#   3. Build a global address-taken set (any `@<name>` that is NOT followed
#      by `(` — the function escapes via a constant pointer or alias and may
#      be invoked at runtime from a context we cannot see).
#   4. Run a fixpoint expansion of init_set / exit_set via the rule:
#         f ∈ init_set ⇐ f has at least one in-IR caller
#                       AND every caller of f is in init_set
#                       AND f ∉ addr_taken (callbacks may run at runtime)
#      Seed init_set / exit_set are the initcall/exitcall function names.
#   5. Drop anything that ended up in BOTH init_set and exit_set (ambiguous
#      lifecycle — better to leave classification to the per-module pass).
#
# The result is two newline-delimited files of function names that mbcheck
# will read via --init-seed-file / --exit-seed-file and union into the
# LifecycleHeuristic seed sets *before* the per-module H27 fixpoint runs.
# ---------------------------------------------------------------------------

_SYM = r"[\w$.]"
_INIT_ASM_RE = re.compile(
    r'__initcall__kmod_\w+?__\d+_\d+_(.+)'
    r'((?:[0-7]|early|rootfs|subsys|device|late|arch|con|fs))s?:')
_EXIT_VAR_RE = re.compile(
    rf'@__exitcall_\w+\s*=\s*internal global ptr @({_SYM}+)')
_DEFINE_RE  = re.compile(rf'^define\s+.*?@({_SYM}+)\s*\(')
_CALLEE_RE  = re.compile(rf'@({_SYM}+)\s*\(')
_ADDR_RE    = re.compile(rf'@({_SYM}+)(?!\s*\()')

_INITCALL_LEVELS = {
    "early", "rootfs", "subsys", "device", "late", "arch", "con", "fs",
    "0", "1", "2", "3", "4", "5", "6", "7",
}


def _parse_one_ll(path):
    """Parse a single .ll file. Returns a tuple
       (initcalls, exitcalls, init_section, exit_section,
        addr_taken, fn_defs, edges_items)
    where edges_items is a list of (caller, frozenset(callees)) tuples
    suitable for cheap merging."""
    initcalls: set[str] = set()
    exitcalls: set[str] = set()
    init_section: set[str] = set()
    exit_section: set[str] = set()
    addr_taken: set[str] = set()
    fn_defs: set[str] = set()
    edges: dict[str, set[str]] = {}

    cur = None
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                # Initcall asm marker (kept ambiguous candidate; resolved later
                # by intersecting against the global fn_defs set).
                m = _INIT_ASM_RE.search(line)
                if m:
                    initcalls.add(m.group(1))
                m = _EXIT_VAR_RE.search(line)
                if m:
                    exitcalls.add(m.group(1))

                stripped = line.lstrip()
                if cur is None:
                    if stripped.startswith("define "):
                        md = _DEFINE_RE.match(stripped)
                        if md:
                            cur = md.group(1)
                            fn_defs.add(cur)
                            edges[cur] = set()
                            # `.init.text` / `.exit.text` annotations on the
                            # define line are an authoritative seed: the
                            # kernel's `__init` / `__exit` macro expands to
                            # this section attribute.
                            if '".init.text"' in line:
                                init_section.add(cur)
                            elif '".exit.text"' in line:
                                exit_section.add(cur)
                        continue
                    if stripped.startswith("declare "):
                        continue
                    # Module scope (globals, aliases, metadata).  Any @name
                    # not followed by '(' is an address-taken reference.
                    for am in _ADDR_RE.finditer(line):
                        addr_taken.add(am.group(1))
                    continue

                # Inside a function body.
                if stripped.startswith("}"):
                    cur = None
                    continue
                callees = edges[cur]
                for cm in _CALLEE_RE.finditer(line):
                    callees.add(cm.group(1))
                for am in _ADDR_RE.finditer(line):
                    addr_taken.add(am.group(1))
    except OSError:
        return set(), set(), set(), set(), set(), set(), []

    return (initcalls, exitcalls, init_section, exit_section,
            addr_taken, fn_defs,
            [(c, frozenset(es)) for c, es in edges.items() if es])


def _resolve_initcall_candidate(token, fn_defs):
    """Given the regex-extracted candidate (funcname concatenated with the
    level token, with optional 's' already stripped by the regex), brute-force
    enumerate plausible (funcname, level) splits and return the funcname
    that exists in fn_defs.  Falls back to the longest-suffix match."""
    # _INIT_ASM_RE captures (funcname-with-trailing-junk, level).  Greedy
    # `(.+)` already consumed everything up to the last level token before
    # `s?:`, so the candidate IS the funcname in nearly every well-formed
    # case.  But if funcname legitimately ends in a digit before the level
    # (e.g. usb2_init4 → funcname=usb2_init, level=4), greedy matching
    # already gave us the right answer.  Accept iff present in fn_defs.
    if token in fn_defs:
        return token
    # Otherwise scan all underscore-delimited suffixes.
    for i, ch in enumerate(token):
        if i > 0 and token[i - 1] == "_":
            cand = token[i:]
            if cand in fn_defs:
                return cand
    return None


def _collect_global_init_seeds(testdir, jobs, out_init, out_exit):
    """Walk testdir, build cross-TU init/exit reachability seeds and write
    two seed files.  Returns (n_init, n_exit, n_files)."""
    from concurrent.futures import ProcessPoolExecutor
    ll_files = sorted(testdir.rglob("*.ll"))
    n_files = len(ll_files)
    if n_files == 0:
        return 0, 0, 0

    print(f"[cross-tu] scanning {n_files} .ll files with {jobs} workers …",
          flush=True)
    t0 = datetime.now()

    initcall_raw: set[str] = set()
    exitcalls:    set[str] = set()
    init_section: set[str] = set()
    exit_section: set[str] = set()
    addr_taken:   set[str] = set()
    fn_defs:      set[str] = set()
    callers:      dict[str, set[str]] = defaultdict(set)

    # Process pool because regex over hundreds of MB doesn't release the GIL
    # consistently and we want disk reads + regex CPU work to overlap.
    if jobs > 1:
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            for ic, ec, isec, esec, at, fd, edges in pool.map(
                    _parse_one_ll, ll_files, chunksize=16):
                initcall_raw |= ic
                exitcalls    |= ec
                init_section |= isec
                exit_section |= esec
                addr_taken   |= at
                fn_defs      |= fd
                for caller, callees in edges:
                    for callee in callees:
                        callers[callee].add(caller)
    else:
        for p in ll_files:
            ic, ec, isec, esec, at, fd, edges = _parse_one_ll(p)
            initcall_raw |= ic
            exitcalls    |= ec
            init_section |= isec
            exit_section |= esec
            addr_taken   |= at
            fn_defs      |= fd
            for caller, callees in edges:
                for callee in callees:
                    callers[callee].add(caller)

    # Resolve raw initcall tokens to real function names.
    initcalls: set[str] = set()
    for tok in initcall_raw:
        name = _resolve_initcall_candidate(tok, fn_defs)
        if name is not None:
            initcalls.add(name)
    # Section-annotated functions are an authoritative additional seed.
    initcalls |= init_section
    exitcalls |= exit_section

    # Restrict exit set to functions actually defined somewhere.
    exitcalls &= fn_defs

    elapsed_scan = (datetime.now() - t0).total_seconds()
    print(f"[cross-tu] scan done in {elapsed_scan:.1f}s: "
          f"{len(fn_defs)} defs, {len(initcalls)} initcalls "
          f"({len(init_section)} via .init.text), "
          f"{len(exitcalls)} exitcalls "
          f"({len(exit_section)} via .exit.text), "
          f"{len(addr_taken)} addr-taken, "
          f"{len(callers)} callees-with-callers", flush=True)

    # Fixpoint expansion.
    def _expand(seed: set[str], label: str) -> set[str]:
        out = set(seed)
        # Iterate using a worklist seeded with potential successors of out:
        # any callee whose caller set is a subset of out.
        # For initial pass, examine every callee.
        candidates = list(callers.keys())
        passes = 0
        while True:
            passes += 1
            new = set()
            for callee in candidates:
                if callee in out:
                    continue
                if callee in addr_taken:
                    continue            # callbacks may run at runtime
                if callee not in fn_defs:
                    continue            # can't reason about external decls
                cset = callers[callee]
                if not cset:
                    continue
                if cset <= out:
                    new.add(callee)
            if not new:
                break
            out |= new
        print(f"[cross-tu] {label} fixpoint: {passes} passes, "
              f"|seed|={len(seed)} → |closure|={len(out)}",
              flush=True)
        return out

    init_set = _expand(initcalls, "init")
    exit_set = _expand(exitcalls, "exit")

    # Strip ambiguous (a function that is both init- and exit-reachable is
    # almost certainly a shared helper that gets called at runtime via a
    # path we missed; better not to suppress its pairs).
    overlap = init_set & exit_set
    if overlap:
        init_set -= overlap
        exit_set -= overlap
        print(f"[cross-tu] dropped {len(overlap)} ambiguous (init∩exit) names",
              flush=True)

    out_init.parent.mkdir(parents=True, exist_ok=True)
    with open(out_init, "w", encoding="utf-8") as f:
        f.write("# cross-TU init-only function names (run_kernel_noLTO.py)\n")
        f.write(f"# generated: {datetime.now().isoformat()}\n")
        f.write(f"# testdir  : {testdir}\n")
        f.write(f"# files    : {n_files}\n")
        for n in sorted(init_set):
            f.write(n)
            f.write("\n")
    with open(out_exit, "w", encoding="utf-8") as f:
        f.write("# cross-TU exit-only function names (run_kernel_noLTO.py)\n")
        f.write(f"# generated: {datetime.now().isoformat()}\n")
        f.write(f"# testdir  : {testdir}\n")
        f.write(f"# files    : {n_files}\n")
        for n in sorted(exit_set):
            f.write(n)
            f.write("\n")

    elapsed_total = (datetime.now() - t0).total_seconds()
    print(f"[cross-tu] wrote {len(init_set)} init / {len(exit_set)} exit "
          f"names in {elapsed_total:.1f}s total", flush=True)
    print(f"[cross-tu]   init-seed-file = {out_init}")
    print(f"[cross-tu]   exit-seed-file = {out_exit}")
    return len(init_set), len(exit_set), n_files


# ---------------------------------------------------------------------------
# Peak resident-set sampler
# ---------------------------------------------------------------------------
# getrusage() reports the high-water mark of the single largest child, which
# understates a sweep that keeps `--jobs` analyses resident at once.  Sample
# the whole process tree instead and keep the largest total seen.
# ---------------------------------------------------------------------------

class RssSampler(threading.Thread):
    def __init__(self, interval=0.2):
        super().__init__(daemon=True)
        self.interval = interval
        self.peak_kb = 0
        self._stop = threading.Event()

    @staticmethod
    def _tree_rss_kb(root_pid):
        children = defaultdict(list)
        rss = {}
        for entry in os.scandir("/proc"):
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            try:
                with open(f"/proc/{pid}/stat", "rb") as fh:
                    fields = fh.read().rsplit(b")", 1)[1].split()
                children[int(fields[1])].append(pid)      # ppid
                rss[pid] = int(fields[21]) * (os.sysconf("SC_PAGE_SIZE") // 1024)
            except (OSError, IndexError, ValueError):
                continue
        total, stack = 0, [root_pid]
        while stack:
            pid = stack.pop()
            total += rss.get(pid, 0)
            stack.extend(children.get(pid, ()))
        return total

    def run(self):
        me = os.getpid()
        while not self._stop.wait(self.interval):
            try:
                self.peak_kb = max(self.peak_kb, self._tree_rss_kb(me))
            except Exception:
                pass

    def stop(self):
        self._stop.set()


def _run_summary(mbcheck, ll_file, timeout):
    try:
        proc = subprocess.run(
            [str(mbcheck), *_MBCHECK_EXTRA, "--summary", str(ll_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=timeout,
        )
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired:
        return -1, f"(TIMEOUT after {timeout}s)\n"
    except Exception as exc:
        return -2, f"(ERROR: {exc})\n"


def _run_litmus(mbcheck, ll_file, outdir, timeout):
    """Generate .litmus files for ll_file into outdir. Returns (exit_code, stdout)."""
    try:
        proc = subprocess.run(
            [str(mbcheck), *_MBCHECK_EXTRA, "--litmus", str(outdir), str(ll_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=timeout,
        )
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired:
        return -1, f"(TIMEOUT after {timeout}s)\n"
    except Exception as exc:
        return -2, f"(ERROR: {exc})\n"


def _run_herd7(herd7_bin, litmus_file, timeout=60):
    """Run herd7 on a single .litmus file. Returns dict with ok, output, exit_code.

    err_kind classification (only meaningful when ok is None):
      "timeout" : herd7 did not complete within `timeout` seconds.
      "parse"   : herd7 rejected the .litmus syntax (no exec'd state).
      "crash"   : non-zero exit but produced no parse-fail signature.
      "other"   : unclassified (e.g. signalled subprocess).
    """
    try:
        argv = [str(herd7_bin)]
        if _HERDLIB:
            argv += ["-set-libdir", _HERDLIB]
        argv.append(str(litmus_file))
        proc = subprocess.run(
            argv,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=timeout,
        )
        ok = None
        observation = ""
        for line in proc.stdout.splitlines():
            s = line.strip()
            if s == "Ok":
                ok = True
            elif s == "No":
                ok = False
            elif s.startswith("Observation "):
                observation = s
        err_kind = ""
        if ok is None:
            combined = (proc.stdout + "\n" + proc.stderr).lower()
            # herd7 prints "fatal" or "syntax error" / "lexical error" /
            # "expecting" for malformed inputs; missing "Test ..." line is a
            # secondary signal of parse failure.
            if ("syntax error" in combined or "lexical error" in combined
                    or "fatal:" in combined or "fatal error" in combined
                    or "parse error" in combined):
                err_kind = "parse"
            elif proc.returncode in (-6, -11, -7, -8):  # SIGABRT/SEGV/BUS/FPE
                err_kind = "crash"
            elif proc.returncode != 0:
                err_kind = "crash"
            else:
                err_kind = "other"
        return {
            "litmus": litmus_file.name,
            "ok": ok,
            "observation": observation,
            "output": proc.stdout,
            "stderr": proc.stderr,
            "exit_code": proc.returncode,
            "err_kind": err_kind,
        }
    except subprocess.TimeoutExpired:
        return {"litmus": litmus_file.name, "ok": None,
                "observation": "", "output": f"(TIMEOUT after {timeout}s)\n",
                "stderr": "", "exit_code": -1, "err_kind": "timeout"}
    except Exception as exc:
        return {"litmus": litmus_file.name, "ok": None,
                "observation": "", "output": f"(ERROR: {exc})\n",
                "stderr": "", "exit_code": -2, "err_kind": "other"}


def _parse_stats(output):
    r = {
        "anchors_total": 0, "anchor_labels": {},
        "causal_total": 0,
        "pairs_total": 0, "pairs_filtered": 0, "pairs_surviving": 0,
        "heuristics": {}, "heuristic_subs": {},
        "litmus_total": 0,
        "phase_ns": defaultdict(int),
    }
    for line in output.splitlines():
        line = line.strip()
        m = _RE_ANCHORS_TOTAL.match(line)
        if m: r["anchors_total"] = int(m.group(1)); continue
        m = _RE_ANCHOR_LABEL.match(line)
        if m: r["anchor_labels"][m.group(1)] = int(m.group(2)); continue
        m = _RE_CAUSAL_TOTAL.match(line)
        if m: r["causal_total"] = int(m.group(1)); continue
        m = _RE_PAIRS_TOTAL.match(line)
        if m: r["pairs_total"] = int(m.group(1)); continue
        m = _RE_PAIRS_FILTERED.match(line)
        if m: r["pairs_filtered"] = int(m.group(1)); continue
        m = _RE_PAIRS_SURVIVE.match(line)
        if m: r["pairs_surviving"] = int(m.group(1)); continue
        m = _RE_HEURISTIC.match(line)
        if m: r["heuristics"][m.group(1)] = int(m.group(2)); continue
        m = _RE_HEURISTIC_SUB.match(line)
        if m:
            hname, subcat, cnt = m.group(1), m.group(2), int(m.group(3))
            r["heuristic_subs"].setdefault(hname, {})[subcat] = \
                r["heuristic_subs"].get(hname, {}).get(subcat, 0) + cnt
            continue
        m = _RE_LITMUS_TOTAL.match(line)
        if m: r["litmus_total"] = int(m.group(1)); continue
        m = _RE_PHASE_NS.match(line)
        if m: r["phase_ns"][m.group(1)] += int(m.group(2))
    return r


def _process_file(mbcheck, ll_file, herd7_bin=None, litmus_base=None,
                  herd7_sem=None, testdir=None, timeout=120):
    _t0 = time.monotonic()
    exit_code, output = _run_summary(mbcheck, ll_file, timeout)
    _mbcheck_ns = int((time.monotonic() - _t0) * 1e9)
    stats = _parse_stats(output)
    result = {"file": ll_file, "exit_code": exit_code, "output": output,
              "herd7_results": [], "herd7_ns": 0,
              "mbcheck_ns": _mbcheck_ns, **stats}

    # Gate on surviving pairs — more robust than litmus_total (which requires
    # a binary new enough to emit STATS:litmus:total).  The actual count is
    # taken from files on disk after --litmus so the aggregation is correct
    # regardless of binary version.
    if herd7_bin and stats["pairs_surviving"] > 0 and litmus_base is not None:
        # Use the path relative to testdir (with '/' → '__') as the litmus
        # subdir name so that distinct .ll files sharing a basename (e.g.
        # kernel/events/core.ll vs net/devlink/core.ll) do not collide.
        # Falls back to bare stem if testdir is not provided (legacy callers).
        if testdir is not None:
            try:
                rel = ll_file.relative_to(testdir).with_suffix("")
                subname = str(rel).replace("/", "__")
            except ValueError:
                subname = ll_file.stem
        else:
            subname = ll_file.stem
        litmus_dir = litmus_base / subname
        litmus_dir.mkdir(parents=True, exist_ok=True)
        _t0 = time.monotonic()
        _, litmus_out = _run_litmus(mbcheck, ll_file, litmus_dir, timeout)
        result["mbcheck_ns"] += int((time.monotonic() - _t0) * 1e9)
        # The sweep runs the whole pipeline a second time to emit the litmus
        # files, so that pass has to be charged to the same phases.
        for phase, ns in _parse_stats(litmus_out)["phase_ns"].items():
            result["phase_ns"][phase] += ns
        litmus_files = sorted(litmus_dir.glob("*.litmus"))
        result["litmus_total"] = len(litmus_files)   # override parsed value
        for lf in litmus_files:
            # herd7_sem is None when limit is 0 (unlimited); acquire(blocking=False)
            # returns False immediately when the budget is exhausted — skip the rest.
            if herd7_sem is not None and not herd7_sem.acquire(blocking=False):
                break
            _t0 = time.monotonic()
            r = _run_herd7(herd7_bin, lf)
            result["herd7_ns"] += int((time.monotonic() - _t0) * 1e9)
            result["herd7_results"].append(r)

    return result


def _count_lines(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().count(b"\n")
    except OSError:
        return 0


def _pct(num, denom):
    if denom == 0: return ""
    return f"({num * 100 / denom:.1f}%)"


def _bar(num, denom, width=20):
    if denom == 0: return " " * width
    filled = round(num * width / denom)
    return "#" * filled + "." * (width - filled)


def main():
    parser = argparse.ArgumentParser(
        description="Run mbcheck --summary on every kernel-noLTO IR module.")
    parser.add_argument("--mbcheck", type=Path, default=DEFAULT_MBCHECK)
    parser.add_argument("--testdir", type=Path, default=DEFAULT_TESTDIR)
    parser.add_argument("--jobs",    type=int,  default=os.cpu_count() or 1)
    parser.add_argument("--log",     type=Path, default=DEFAULT_LOG)
    parser.add_argument("--limit",       type=int,  default=None, metavar="N")
    parser.add_argument("--limit-herd7", type=int,  default=0,   metavar="N",
                        help="Max total herd7 invocations (default: unlimited, 0=unlimited)")
    parser.add_argument("--herd7",   type=Path, default=None,
                        help="Path to the herd7 binary (default: $HERD7, "
                             "else the first herd7 on $PATH)")
    parser.add_argument("--litmusdir", type=Path, default=None,
                        help="Directory for generated .litmus files "
                             "(default: <log parent>/litmus)")
    parser.add_argument("--disable-heuristic", action="append", default=[],
                        metavar="NAME",
                        help="Forward --disable-heuristic=NAME to mbcheck (may "
                             "be repeated; comma-separated also accepted; "
                             "'all' disables every heuristic).")
    parser.add_argument("--strict-publish", action="store_true",
                        help="Forward --strict-publish to mbcheck. Synthesises "
                             "plain-publish anchor brackets for MP-shape "
                             "stores lacking a release barrier (Action 2).")
    parser.add_argument("--cross-tu-init", action="store_true",
                        help="v14 path-1: pre-scan the entire testdir to "
                             "build a global __initcall reachability closure, "
                             "write seed files, and forward "
                             "--init-seed-file/--exit-seed-file to every "
                             "mbcheck invocation.")
    parser.add_argument("--init-seed-out", type=Path, default=None,
                        help="Override path for the cross-TU init seed file "
                             "(default: <log parent>/global_init_seeds.txt).")
    parser.add_argument("--exit-seed-out", type=Path, default=None,
                        help="Override path for the cross-TU exit seed file "
                             "(default: <log parent>/global_exit_seeds.txt).")
    parser.add_argument("--no-herd7", action="store_true",
                        help="Skip herd7 validation entirely")
    parser.add_argument("--json", type=Path, default=None, metavar="FILE",
                        help="Also write the aggregate results as JSON")
    parser.add_argument("--timeout", type=int, default=120, metavar="SEC",
                        help="Per-module time limit (default: %(default)s). "
                             "A module that exceeds it contributes nothing, so "
                             "counts depend on this value and on machine speed.")
    args = parser.parse_args()

    if not args.mbcheck.is_file():
        sys.exit(f"Error: mbcheck not found: {args.mbcheck}\nBuild first: cd build && make")
    if not args.testdir.is_dir():
        sys.exit(
            f"Error: IR corpus not found: {args.testdir}\n"
            "  The corpus is not shipped in the repository.  Build it with\n"
            "    scripts/build-kernel-ir.sh <kernel-source> <output-dir>\n"
            "  and pass --testdir <output-dir>.  See README.md.")

    # T2.1 — wire ablation flags into worker processes.
    global _MBCHECK_EXTRA
    flat_disable: list[str] = []
    for item in args.disable_heuristic:
        flat_disable.extend([s.strip() for s in item.split(",") if s.strip()])
    if flat_disable:
        _MBCHECK_EXTRA = ["--disable-heuristic=" + ",".join(flat_disable)]
        print(f"[run_kernel_noLTO] ablation: disabling heuristics = {flat_disable}")

    # Action 2 — wire --strict-publish flag into worker processes.
    if args.strict_publish:
        _MBCHECK_EXTRA = list(_MBCHECK_EXTRA) + ["--strict-publish"]
        print("[run_kernel_noLTO] strict-publish: synthesising plain-publish brackets")

    # v14 path-1 — cross-TU init/exit reachability pre-pass.
    if args.cross_tu_init:
        out_init = args.init_seed_out or (args.log.parent / "global_init_seeds.txt")
        out_exit = args.exit_seed_out or (args.log.parent / "global_exit_seeds.txt")
        _collect_global_init_seeds(args.testdir, args.jobs, out_init, out_exit)
        _MBCHECK_EXTRA = list(_MBCHECK_EXTRA) + [
            f"--init-seed-file={out_init}",
            f"--exit-seed-file={out_exit}",
        ]
        print(f"[run_kernel_noLTO] cross-tu-init: forwarding seed files "
              f"to mbcheck via _MBCHECK_EXTRA")

    # Resolve herd7 settings
    global _HERDLIB
    herd7_bin = None
    if not args.no_herd7:
        herd7_bin = _resolve_herd7(args.herd7)
        if herd7_bin is None or not herd7_bin.is_file():
            sys.exit(
                "Error: herd7 not found"
                + (f" at {herd7_bin}" if herd7_bin else "")
                + ".\n"
                "  Pass --herd7 /path/to/herd7, set $HERD7, or put herd7 on "
                "$PATH.\n"
                "  --no-herd7 skips litmus validation, but Tables 4 and 9 "
                "need it.\n"
                "  See README.md.")
        _HERDLIB = _resolve_herdlib(herd7_bin)
        problem = _check_herd7(herd7_bin)
        if problem:
            sys.exit(f"Error: {problem}\n  See README.md.")

    litmus_base = None
    herd7_sem = None
    if herd7_bin is not None:
        litmus_base = args.litmusdir if args.litmusdir else args.log.parent / "litmus"
        litmus_base.mkdir(parents=True, exist_ok=True)
        if args.limit_herd7 > 0:
            herd7_sem = threading.Semaphore(args.limit_herd7)

    ll_files = sorted(args.testdir.rglob("*.ll"))
    if not ll_files:
        sys.exit(f"Error: no .ll files found under {args.testdir}")
    if args.limit:
        ll_files = ll_files[:args.limit]

    total_files = len(ll_files)
    print(f"mbcheck : {args.mbcheck}")
    print(f"testdir : {args.testdir}")
    print(f"log     : {args.log}")
    print(f"files   : {total_files}")
    print(f"workers : {args.jobs}")
    if herd7_bin:
        print(f"herd7   : {herd7_bin}")
        print(f"litmusd : {litmus_base}")
        print(f"herdlib : {_HERDLIB or '(herd7 default)'}")
        limit_herd7_str = str(args.limit_herd7) if args.limit_herd7 > 0 else "unlimited"
        print(f"herd7lim: {limit_herd7_str}")
    else:
        print("herd7   : (disabled)")
    print()

    results = []
    done = 0
    args.log.parent.mkdir(parents=True, exist_ok=True)

    rss_sampler = RssSampler()
    rss_sampler.start()
    t_scan0 = time.monotonic()

    with open(args.log, "w", encoding="utf-8") as logf:
        logf.write("mbcheck kernel-noLTO sweep\n")
        logf.write(f"started  : {datetime.now().isoformat()}\n")
        logf.write(f"mbcheck  : {args.mbcheck}\n")
        logf.write(f"testdir  : {args.testdir}\n")
        logf.write(f"files    : {total_files}\n")
        logf.write("=" * 80 + "\n\n")

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(_process_file, args.mbcheck, f,
                                   herd7_bin, litmus_base, herd7_sem,
                                   args.testdir, args.timeout): f
                       for f in ll_files}
            for fut in as_completed(futures):
                done += 1
                r = fut.result()
                results.append(r)
                rel = r["file"].relative_to(args.testdir)

                logf.write("=" * 80 + "\n")
                logf.write(f"FILE : {rel}\n")
                if r["exit_code"] not in (0, None):
                    logf.write(f"  exit_code={r['exit_code']}\n")
                logf.write(
                    f"  anchors={r['anchors_total']}  causal={r['causal_total']}  "
                    f"pairs_total={r['pairs_total']}  pairs_filtered={r['pairs_filtered']}  "
                    f"pairs_surviving={r['pairs_surviving']}  litmus={r['litmus_total']}\n"
                )
                for lbl, cnt in sorted(r["anchor_labels"].items(), key=lambda x: -x[1]):
                    logf.write(f"    anchor  {cnt:>4}  {lbl}\n")
                for hname, cnt in sorted(r["heuristics"].items(), key=lambda x: -x[1]):
                    logf.write(f"    heur    {cnt:>4}  {hname}\n")
                for hr in r["herd7_results"]:
                    status = "OK " if hr["ok"] is True else ("NO " if hr["ok"] is False else "ERR")
                    logf.write(f"    herd7   {status}  {hr['litmus']}")
                    if hr["observation"]:
                        logf.write(f"  [{hr['observation']}]")
                    logf.write("\n")
                    if hr["ok"] is not True:
                        logf.write(f"      stderr: {hr['stderr'].strip()}\n")
                        logf.write(f"      output: {hr['output'].strip()}\n")
                logf.write("\n[--summary output]\n")
                logf.write(r["output"])
                logf.write("\n")
                logf.flush()

                pct_done = done * 100 // total_files
                print(f"\r[{done:>4}/{total_files}  {pct_done:>3}%]  {str(rel):<60}",
                      end="", flush=True)

    t_scan1 = time.monotonic()

    print()

    # ---- Aggregation -------------------------------------------------------
    total_anchors   = sum(r["anchors_total"]   for r in results)
    total_causal    = sum(r["causal_total"]    for r in results)
    total_pairs     = sum(r["pairs_total"]     for r in results)
    total_filtered  = sum(r["pairs_filtered"]  for r in results)
    total_surviving = sum(r["pairs_surviving"] for r in results)
    total_litmus    = sum(r["litmus_total"]    for r in results)

    all_herd7       = [hr for r in results for hr in r["herd7_results"]]
    herd7_ok        = sum(1 for hr in all_herd7 if hr["ok"] is True)
    herd7_no        = sum(1 for hr in all_herd7 if hr["ok"] is False)
    herd7_err       = sum(1 for hr in all_herd7 if hr["ok"] is None)
    # G7.1: disaggregate Err into parse / timeout / crash / other
    herd7_err_parse   = sum(1 for hr in all_herd7
                             if hr["ok"] is None and hr.get("err_kind") == "parse")
    herd7_err_timeout = sum(1 for hr in all_herd7
                             if hr["ok"] is None and hr.get("err_kind") == "timeout")
    herd7_err_crash   = sum(1 for hr in all_herd7
                             if hr["ok"] is None and hr.get("err_kind") == "crash")
    herd7_err_other   = (herd7_err - herd7_err_parse - herd7_err_timeout
                         - herd7_err_crash)

    # G7.2: cross-LL duplicate-pair suppression for reporting.
    # Key each "No" verdict by (anchor_fn, partner_fn, anchor_label) extracted
    # from the .litmus file's leading comment lines.  Many drivers contain the
    # same generic helper (e.g. seqcount_lockdep_reader_access) which produces
    # identical pairs across many .ll modules — these inflate the "No" count
    # without representing distinct bugs.
    def _herd7_signature(litmus_path):
        try:
            with open(litmus_path, "r") as f:
                anchor_label = ""
                anchor_fn = ""
                partner_fn = ""
                for line in f:
                    line = line.strip()
                    if line.startswith("(* anchor:"):
                        # "(* anchor: <label>  [..] (..) *)"
                        a = line[len("(* anchor:"):].split("[", 1)[0].strip()
                        anchor_label = a
                    elif line.startswith("(* anchor-fn:"):
                        # "(* anchor-fn: <fn>  partner-fn: <fn> *)"
                        tail = line[len("(* anchor-fn:"):]
                        if "partner-fn:" in tail:
                            af, pf = tail.split("partner-fn:", 1)
                            anchor_fn = af.strip()
                            partner_fn = pf.replace("*)", "").strip()
                        if anchor_label or anchor_fn:
                            return (anchor_fn, partner_fn, anchor_label)
                    elif line and not line.startswith("(*"):
                        break
                return (anchor_fn, partner_fn, anchor_label)
        except OSError:
            return ("", "", "")

    files_with_anchors   = sum(1 for r in results if r["anchors_total"]   > 0)
    files_with_pairs     = sum(1 for r in results if r["pairs_total"]     > 0)
    files_with_surviving = sum(1 for r in results if r["pairs_surviving"] > 0)
    files_with_litmus    = sum(1 for r in results if r["litmus_total"]    > 0)
    files_with_errors    = sum(1 for r in results if r["exit_code"] not in (0, None))

    anchor_labels = defaultdict(int)
    for r in results:
        for lbl, cnt in r["anchor_labels"].items():
            anchor_labels[lbl] += cnt

    heuristic_totals = defaultdict(int)
    heuristic_sub_totals = defaultdict(lambda: defaultdict(int))
    for r in results:
        for hname, cnt in r["heuristics"].items():
            heuristic_totals[hname] += cnt
        for hname, subcats in r["heuristic_subs"].items():
            for subcat, cnt in subcats.items():
                heuristic_sub_totals[hname][subcat] += cnt

    t_report0 = time.monotonic()

    # ---- Build report ------------------------------------------------------
    W    = 72
    sep  = "=" * W
    sep2 = "-" * W

    L = []
    def h(text=""):  L.append(f"  {text}")
    def s():         L.append("")

    L.append("")
    L.append(sep)
    L.append("  MBCHECK  KERNEL-noLTO  SWEEP  --  SUMMARY REPORT")
    L.append(f"  Generated : {datetime.now().isoformat()}")
    L.append(sep)

    all_nos  = [(r, hr) for r in results for hr in r["herd7_results"] if hr["ok"] is False]
    all_errs = [(r, hr) for r in results for hr in r["herd7_results"] if hr["ok"] is None]

    # --- Section 1 ---
    s(); h("1. IR MODULES SCANNED"); h(sep2)
    h(f"  Total .ll files processed        : {total_files:>6}")
    h(f"  Total anchors detected           : {total_anchors:>6}")
    h(f"  Total causal brackets identified : {total_causal:>6}")
    h(f"  Total fn-pairs detected (raw)    : {total_pairs:>6}")
    h(f"  Total fn-pairs surviving         : {total_surviving:>6}  {_pct(total_surviving, total_pairs)}")
    h(f"  Total herd7 litmus files         : {total_litmus:>6}  (across {files_with_litmus} modules)")
    if files_with_errors:
        h(f"  Files with errors / timeouts     : {files_with_errors:>6}")

    # --- Section 2 ---
    s(); h("2. ANCHORS FOUND"); h(sep2)
    h(f"  Total anchors detected           : {total_anchors:>6}")
    s(); h("  Per-label breakdown (most to least):")
    if anchor_labels:
        sorted_labels = sorted(anchor_labels.items(), key=lambda x: -x[1])
        lw = max(len(lbl) for lbl, _ in sorted_labels)
        for lbl, cnt in sorted_labels:
            h(f"    {lbl:<{lw}}  {cnt:>5}  {_pct(cnt, total_anchors):>8}  {_bar(cnt, total_anchors)}")
    else:
        h("    (none)")

    # --- Section 3 ---
    s(); h("3. CAUSAL ALLOCATIONS IDENTIFIED"); h(sep2)
    h(f"  Total causal brackets identified : {total_causal:>6}")

    # --- Section 4 ---
    s(); h("4. FN-PAIRS IDENTIFIED  (raw, before filtering)"); h(sep2)
    h(f"  Total fn-pairs detected          : {total_pairs:>6}")

    # --- Section 5 ---
    s(); h("5. FN-PAIRS FILTERED"); h(sep2)
    h(f"  Total fn-pairs filtered          : {total_filtered:>6}  {_pct(total_filtered, total_pairs)}")
    s(); h("  Heuristics (most to least used):")
    if heuristic_totals:
        sorted_h = sorted(heuristic_totals.items(), key=lambda x: -x[1])
        hw = max(len(hn) for hn, _ in sorted_h)
        for hname, cnt in sorted_h:
            h(f"    {hname:<{hw}}  {cnt:>5}  {_pct(cnt, total_filtered):>8}  {_bar(cnt, total_filtered)}")
            # Sub-breakdown: top 10 sub-categories for this heuristic
            subcats = heuristic_sub_totals.get(hname, {})
            if subcats:
                sorted_sub = sorted(subcats.items(), key=lambda x: -x[1])[:10]
                sw = max(len(s) for s, _ in sorted_sub)
                for subcat, scnt in sorted_sub:
                    h(f"      {subcat:<{sw}}  {scnt:>5}  {_pct(scnt, cnt):>8}")
    else:
        h("    (none -- no pairs were filtered)")

    # --- Section 6 ---
    s(); h("6. FN-PAIRS NOT FILTERED  (surviving / reported)"); h(sep2)
    h(f"  Total fn-pairs surviving         : {total_surviving:>6}  {_pct(total_surviving, total_pairs)}")

    top_surviving = [r for r in sorted(results, key=lambda r: -r["pairs_surviving"])
                     if r["pairs_surviving"] > 0][:15]
    if top_surviving:
        s(); h("  Top files by surviving fn-pairs:")
        h(f"  {'File':<52}  {'raw':>5}  {'filt':>5}  {'surv':>5}")
        h(sep2[:68])
        for r in top_surviving:
            rel = str(r["file"].relative_to(args.testdir))
            h(f"  {rel:<52}  {r['pairs_total']:>5}  {r['pairs_filtered']:>5}  {r['pairs_surviving']:>5}")

    # --- Section 7 ---
    s(); h("7. HERD7 LITMUS FILES GENERATED"); h(sep2)
    h(f"  Total litmus files generated     : {total_litmus:>6}")
    h(f"  Modules with litmus output       : {files_with_litmus:>6}  {_pct(files_with_litmus, total_files)}")

    # Top-N by litmus files
    top_litmus = [r for r in sorted(results, key=lambda r: -r["litmus_total"])
                  if r["litmus_total"] > 0][:15]
    if top_litmus:
        s(); h("  Top files by litmus count:")
        h(f"  {'File':<52}  {'surv':>5}  {'litmus':>6}")
        h(sep2[:68])
        for r in top_litmus:
            rel = str(r["file"].relative_to(args.testdir))
            h(f"  {rel:<52}  {r['pairs_surviving']:>5}  {r['litmus_total']:>6}")

    # --- Section 8 ---
    herd7_limit_note = ""
    if herd7_bin and args.limit_herd7 > 0:
        herd7_limit_note = f"  (capped at --limit-herd7 {args.limit_herd7})"
    s(); h(f"8. HERD7 SIMULATOR VALIDATION  (AArch64 model){herd7_limit_note}"); h(sep2)
    if not all_herd7:
        h("  (no herd7 results -- run without --no-herd7 and with a valid herd7 binary)")
    else:
        h(f"  Total litmus tests run          : {len(all_herd7):>6}")
        h(f"  Ok  (forbidden behaviour absent): {herd7_ok:>6}  {_pct(herd7_ok,  len(all_herd7))}  -- ordering holds")
        h(f"  No  (forbidden behaviour seen)  : {herd7_no:>6}  {_pct(herd7_no,  len(all_herd7))}  -- ordering VIOLATED")
        h(f"  Err (parse / tool error)        : {herd7_err:>6}  {_pct(herd7_err, len(all_herd7))}")
        if herd7_err:
            h(f"     - parse failures             : {herd7_err_parse:>6}")
            h(f"     - timeouts (>60s)            : {herd7_err_timeout:>6}")
            h(f"     - crashes (SIGSEGV / abort)  : {herd7_err_crash:>6}")
            h(f"     - other                      : {herd7_err_other:>6}")

        # G7.2: distinct "No" signatures (deduplicated cross-module).
        no_sigs = {}
        for r, hr in all_nos:
            try:
                rel = r["file"].relative_to(args.testdir).with_suffix("")
                subname = str(rel).replace("/", "__")
            except ValueError:
                subname = r["file"].stem
            litmus_path = (Path(args.litmusdir) if args.litmusdir else
                           args.log.parent / "litmus") / subname / hr["litmus"]
            sig = _herd7_signature(litmus_path)
            no_sigs.setdefault(sig, []).append((r, hr))
        if no_sigs:
            h(f"  Distinct ordering-violation signatures: {len(no_sigs):>4}  "
              f"(of {herd7_no} total)")
        if not all_nos and not all_errs:
            s(); h("  All litmus tests PASSED (Ok) -- all modelled orderings hold.")
        else:
            s(); h("  (see HERD7 QUICK-LOOK at the bottom of this report for the full violations list)")

        # Per-module breakdown (only modules that had herd7 results)
        modules_with_herd7 = [r for r in results if r["herd7_results"]]
        if modules_with_herd7:
            s(); h("  Per-module results:")
            h(f"  {'File':<52}  {'ok':>4}  {'no':>4}  {'err':>4}")
            h(sep2[:68])
            for r in sorted(modules_with_herd7,
                            key=lambda r: str(r["file"])):
                rel = str(r["file"].relative_to(args.testdir))
                mok  = sum(1 for hr in r["herd7_results"] if hr["ok"] is True)
                mno  = sum(1 for hr in r["herd7_results"] if hr["ok"] is False)
                merr = sum(1 for hr in r["herd7_results"] if hr["ok"] is None)
                h(f"  {rel:<52}  {mok:>4}  {mno:>4}  {merr:>4}")

    # --- Quick-look: violations digest (shown last so `tail` reaches it) ---
    if all_herd7:
        s(); h("HERD7 QUICK-LOOK"); h(sep2)
        h(f"  Litmus run: {len(all_herd7)}   Ok: {herd7_ok}   No: {herd7_no}   Err: {herd7_err}")
        if all_errs:
            s(); h("  !! ILL-FORMED LITMUS (generation bug):")
            h(sep2[:68])
            for r, hr in all_errs:
                rel = str(r["file"].relative_to(args.testdir))
                h(f"  [Err]  {rel}  \u2192  {hr['litmus']}")
                for eline in (hr["stderr"] or hr["output"] or "").strip().splitlines()[:4]:
                    h(f"           {eline}")
        if all_nos:
            s(); h(f"  ORDERING VIOLATIONS ({herd7_no} litmus tests):")
            h(sep2[:68])
            for r, hr in all_nos:
                rel = str(r["file"].relative_to(args.testdir))
                h(f"  [No]  {rel}  \u2192  {hr['litmus']}")
                if hr["observation"]:
                    h(f"        {hr['observation']}")
        elif not all_errs:
            s(); h("  All litmus tests PASSED (Ok).")

    # ---------------------------------------------------------------------
    # 9. RUNTIME BREAKDOWN
    # ---------------------------------------------------------------------
    # The six analysis phases are measured inside mbcheck and summed over all
    # worker processes, so they are CPU time, not wall clock.  herd7 is timed
    # the same way.  Converting to a wall-clock column means splitting the
    # measured wall clock of the scan in proportion to that CPU time, which
    # holds as long as the workers stay busy.  The report phase is serial, so
    # its wall clock is measured directly.
    PHASE_ORDER = [
        ("ir_load",            "IR parse / link"),
        ("summarize_extfn",    "Summarize ext-fn"),
        ("anchor_db",          "Anchor database"),
        ("bracket_extraction", "Bracket extraction"),
        ("function_pairing",   "Function pairing"),
        ("mutex_filtering",    "Mutual exclusion filtering"),
        ("transpiler",         "Transpiler + verifier"),
    ]

    phase_ns = defaultdict(int)
    for r in results:
        for pname, ns in r["phase_ns"].items():
            phase_ns[pname] += ns
    herd7_ns   = sum(r["herd7_ns"] for r in results)
    mbcheck_ns = sum(r["mbcheck_ns"] for r in results)
    # A module killed by the per-module timeout never prints its phase lines,
    # so its time is real but unattributable.  What is left after removing it
    # is the gap between a worker's wall clock and the CPU it actually got,
    # which is scheduling contention: 16 workers share 8 physical cores.
    timeout_ns = sum(r["mbcheck_ns"] for r in results
                     if r["exit_code"] not in (0, None))
    residual_ns = max(0, mbcheck_ns - sum(phase_ns.values()) - timeout_ns)
    child_cpu_s = (resource.getrusage(resource.RUSAGE_CHILDREN).ru_utime +
                   resource.getrusage(resource.RUSAGE_CHILDREN).ru_stime)

    scan_wall   = t_scan1 - t_scan0
    report_wall = time.monotonic() - t_report0
    total_wall  = scan_wall + report_wall
    cpu_ns      = sum(phase_ns.values()) + herd7_ns

    def _mmss(seconds):
        return f"{int(seconds) // 60}:{int(seconds) % 60:02d}"

    rss_sampler.stop()

    s(); h("9. RUNTIME BREAKDOWN"); h(sep2)
    h(f"    Wall clock, whole sweep      : {_mmss(total_wall)}")
    h(f"    Workers                      : {args.jobs}")
    h(f"    Peak RSS, process tree       : {rss_sampler.peak_kb / 1048576:.2f} GB")
    h(f"    Child CPU (getrusage)        : {_mmss(child_cpu_s)}")
    h(f"    Attributed to phases + herd7 : {_mmss(cpu_ns / 1e9)}")
    h(f"    In modules that timed out    : {_mmss(timeout_ns / 1e9)}")
    h(f"    Scheduling contention        : {_mmss(residual_ns / 1e9)}")
    s()
    h("    Shares below are of attributed time only; the wall column splits")
    h(f"    the {_mmss(scan_wall)} scan in the same proportion.")
    s()
    h(f"    {'Component':<28}{'CPU (m:ss)':>12}{'Wall (m:ss)':>13}{'Share':>9}")
    h("    " + "-" * 62)

    rows = []
    for key, label in PHASE_ORDER:
        ns = phase_ns.get(key, 0)
        rows.append((label, ns / 1e9, (ns / cpu_ns) * scan_wall if cpu_ns else 0.0))
    rows.append(("Herd7 analysis", herd7_ns / 1e9,
                 (herd7_ns / cpu_ns) * scan_wall if cpu_ns else 0.0))
    other_ns = phase_ns.get("other", 0)
    rows.append(("Other (setup / teardown)", other_ns / 1e9,
                 (other_ns / cpu_ns) * scan_wall if cpu_ns else 0.0))
    rows.append(("Report + patch", report_wall, report_wall))

    for label, cpu_s, wall_s in rows:
        share = (wall_s / total_wall * 100) if total_wall else 0.0
        h(f"    {label:<28}{_mmss(cpu_s):>12}{_mmss(wall_s):>13}{share:>8.2f}%")
    h("    " + "-" * 62)
    h(f"    {'Total':<28}{_mmss(cpu_ns / 1e9):>12}{_mmss(total_wall):>13}{100.0:>8.2f}%")

    s()
    L.append(sep)
    report = "\n".join(L)

    print(report)
    with open(args.log, "a", encoding="utf-8") as logf:
        logf.write(report + "\n")
    print(f"\nDetailed log written to: {args.log}")

    # Machine-readable form of the report, read by scripts/report.py.
    if args.json:
        summary = {
            "testdir":        str(args.testdir),
            "mbcheck":        str(args.mbcheck),
            "jobs":           args.jobs,
            "ir_modules":     total_files,
            "ir_lines":       sum(_count_lines(r["file"]) for r in results),
            "timing": {
                "wall_total_s":   total_wall,
                "wall_scan_s":    scan_wall,
                "wall_report_s":  report_wall,
                "cpu_attributed_s": cpu_ns / 1e9,
                "cpu_child_s":    child_cpu_s,
                "cpu_timeout_s":  timeout_ns / 1e9,
                "cpu_residual_s": residual_ns / 1e9,
                "peak_rss_gb":    rss_sampler.peak_kb / 1048576,
                "phases_s":       {k: v / 1e9 for k, v in phase_ns.items()},
                "herd7_s":        herd7_ns / 1e9,
                "shares": {
                    **{k: (v / cpu_ns * 100.0 if cpu_ns else 0.0)
                       for k, v in phase_ns.items()},
                    "herd7": (herd7_ns / cpu_ns * 100.0 if cpu_ns else 0.0),
                },
            },
            "anchors":        total_anchors,
            "causal":         total_causal,
            "pairs_total":    total_pairs,
            "pairs_filtered": total_filtered,
            "pairs_surviving": total_surviving,
            "litmus":         total_litmus,
            "modules_with_litmus": files_with_litmus,
            "modules_with_errors": files_with_errors,
            "herd7_ok":       herd7_ok,
            "herd7_no":       herd7_no,
            "herd7_err":      herd7_err,
            "anchor_labels":  dict(anchor_labels),
            "heuristics":     dict(heuristic_totals),
            "violations": [
                {
                    "module":      str(r["file"].relative_to(args.testdir)),
                    "litmus":      hr["litmus"],
                    "observation": hr["observation"],
                }
                for r, hr in all_nos
            ],
            "timed_out_modules": sorted(
                str(r["file"].relative_to(args.testdir))
                for r in results if r["exit_code"] not in (0, None)
            ),
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as jf:
            json.dump(summary, jf, indent=2, sort_keys=True)
        print(f"JSON summary written to : {args.json}")


if __name__ == "__main__":
    main()
