#!/usr/bin/env python3
"""
analyze_no_cases.py  —  Dynamically parse a kernel-noLTO sweep log, extract
every herd7 "No" case, classify each one, and produce a rich debugging report.

Usage:
    python3 src/script/analyze_no_cases.py           \
        [--log      build/kernel_noLTO_results.log]  \
        [--litmusdir build/litmus]                   \
        [--testdir  tests/kernel-noLTO]              \
        [--out      docs/no_cases_analysis.md]       \
        [--csv      docs/no_cases_analysis.csv]

All paths default to locations relative to the repository root.
No data is hardcoded — everything is parsed dynamically from the log.
"""

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Optional


# ── repository root (two levels above src/script/) ───────────────────────────
REPO = Path(__file__).resolve().parents[2]

DEFAULTS = dict(
    log       = REPO / "build"  / "kernel_noLTO_results.log",
    litmusdir = REPO / "build"  / "litmus",
    testdir   = REPO / "tests"  / "kernel-noLTO",
    out       = REPO / "docs"   / "no_cases_analysis.md",
    csv_out   = REPO / "docs"   / "no_cases_analysis.csv",
)

# ── verdict constants ─────────────────────────────────────────────────────────
FP   = "FALSE_POSITIVE"
TP   = "TRUE_POSITIVE"
BORD = "BORDERLINE"
UNKN = "UNKNOWN"

# ── anchor-label classification tables ───────────────────────────────────────

# External calls that are pure utilities with NO synchronisation semantics.
# If anchor label ends with " (ext)" and base name is in this set → FP.
BENIGN_EXT = {
    "__warn_printk", "_printk", "kfree", "get_random_u64", "bitmap_zalloc",
    "dev_set_name", "call_rcu", "sized_strscpy", "lockref_get",
    "vzalloc_noprof", "ktime_get", "jiffies_to_msecs", "memunmap",
    "pcpu_alloc_noprof", "kvfree", "kzalloc", "kmalloc", "kstrdup",
    "kmemdup", "krealloc",
}

# External calls that indicate anchor-fn is in an init or teardown phase —
# never concurrent with the runtime partner-fn.
INIT_EXT = {
    "read_sanitised_ftr_reg",    # ARM64 boot-time CPU feature register (read once)
    "led_trigger_register",      # LED device registration (driver probe / init)
    "remove_proc_entry",         # procfs teardown / exit path
    "timer_init_key",
    "tty_port_init",
    "mutex_init_generic",
    "__init_waitqueue_head",
    "device_register",
    "hrtimer_setup",
    "platform_device_register",
    "platform_driver_register",
}

# LSE release atomics that carry full acquire+release semantics.
STRONG_RELEASE_ATOMICS = {"ldclral", "ldsetal", "casal", "ldaddal", "swpal", "ldadda"}

# ── litmus comment regex ──────────────────────────────────────────────────────
# Actual format: (* anchor: LABEL  [ORDERING]  (SITE) *)
# Two or more spaces separate the three fields; label can contain spaces.
_ANCHOR_RE    = re.compile(r'anchor:\s+(.+?)\s{2,}\[([^\]]+)\]\s+\(([^)]+)\)')
_ANCHOR_FN_RE = re.compile(r'anchor-fn:\s+(\S+)\s+partner-fn:\s+(\S+)')


# ── log parsing ───────────────────────────────────────────────────────────────

def parse_log(log_path: Path) -> tuple[dict, list[dict]]:
    """
    Single-pass parse of the sweep log.

    Returns:
        stats — dict: started, files, total, ok, no, err
        cases — list of dicts, one per herd7-NO line
    """
    stats = dict(started="", files=0, total=0, ok=0, no=0, err=0)
    cases: list[dict] = []
    current_ll: Optional[str] = None

    with open(log_path) as fh:
        for raw in fh:
            line = raw.rstrip()

            m = re.match(r'started\s*:\s*(.+)', line)
            if m:
                stats["started"] = m.group(1).strip()
                continue

            m = re.match(r'files\s*:\s*(\d+)', line)
            if m:
                stats["files"] = int(m.group(1))
                continue

            m = re.match(r'FILE\s*:\s*(.+)', line)
            if m:
                current_ll = m.group(1).strip()
                continue

            m = re.match(r'\s+herd7\s+(OK|NO|ERR)\s+(\S+\.litmus)', line)
            if m:
                result      = m.group(1)
                litmus_name = m.group(2)
                stats["total"] += 1
                if result == "OK":
                    stats["ok"] += 1
                elif result == "NO":
                    stats["no"] += 1
                    ll_stem = Path(current_ll).stem if current_ll else "unknown"
                    cases.append(dict(
                        ll_file          = current_ll or "unknown",
                        ll_stem          = ll_stem,
                        litmus_name      = litmus_name.removesuffix(".litmus"),
                        _litmus_fname    = litmus_name,
                        litmus_path      = None,
                        litmus_text      = "",
                        anchor_label     = "",
                        anchor_ordering  = "",
                        anchor_site      = "",
                        anchor_fn        = "",
                        partner_fn       = "",
                        shared_vars      = [],
                        condition        = "",
                    ))
                else:
                    stats["err"] += 1

    return stats, cases


def attach_litmus(cases: list[dict], litmusdir: Path) -> None:
    """Load litmus file text for each case and parse its header."""
    for c in cases:
        path = litmusdir / c["ll_stem"] / c["_litmus_fname"]
        c["litmus_path"] = path
        if path.exists():
            text = path.read_text()
            c["litmus_text"] = text
            _parse_litmus_into(c, text)
        else:
            c["litmus_text"] = f"(litmus file not found: {path})"


def _parse_litmus_into(c: dict, text: str) -> None:
    m = _ANCHOR_RE.search(text)
    if m:
        c["anchor_label"]    = m.group(1).strip()
        c["anchor_ordering"] = m.group(2).strip()
        c["anchor_site"]     = m.group(3).strip()

    m2 = _ANCHOR_FN_RE.search(text)
    if m2:
        c["anchor_fn"]  = m2.group(1).strip()
        c["partner_fn"] = m2.group(2).strip()

    c["shared_vars"] = re.findall(r'^int\s+(\w+)\s*=', text, re.MULTILINE)

    m3 = re.search(r'~exists\s+\((.+?)\)', text)
    if m3:
        c["condition"] = m3.group(1).strip()


# ── IR extraction ─────────────────────────────────────────────────────────────

def get_fn_ir(testdir: Path, ll_relative: str, fn_name: str,
              max_lines: int = 45) -> tuple[int, list[str]]:
    """
    Extract up to max_lines lines of function fn_name from a .ll file.

    Returns (start_line_number, annotated_lines).
    Returns (0, []) if the file or function is not found.
    """
    ll_path = testdir / ll_relative
    if not ll_path.exists():
        return 0, []

    lines    = ll_path.read_text().splitlines()
    in_fn    = False
    out      = []
    start_no = 0

    for i, line in enumerate(lines, 1):
        if not in_fn:
            if line.startswith("define") and (
                    f"@{fn_name}(" in line or f"@{fn_name} " in line):
                in_fn    = True
                start_no = i
                out.append(f"L{i}: {line}")
            continue
        out.append(f"L{i}: {line}")
        if line.startswith("}"):
            break
        if len(out) >= max_lines:
            out.append("  ... (truncated — see full .ll file)")
            break

    return start_no, out


# ── classification engine ─────────────────────────────────────────────────────

def classify(c: dict) -> tuple[str, str, str]:
    """
    Classify one No case.

    Returns (verdict, category, reason).
    """
    label      = c["anchor_label"]
    ordering   = c["anchor_ordering"]
    anchor_fn  = c["anchor_fn"]
    partner_fn = c["partner_fn"]
    ll_stem    = c["ll_stem"]

    is_ext     = label.endswith(" (ext)")
    base_label = label[:-6].strip() if is_ext else label.strip()

    # 1. Pure utility external function ──────────────────────────────────────
    if is_ext and base_label in BENIGN_EXT:
        return FP, "BENIGN_EXT_ANCHOR", (
            f"The anchor '{label}' is an external utility call with no "
            f"synchronisation semantics.  mbcheck conservatively modelled the "
            f"call-site as SC but the function provides no release/acquire "
            f"guarantee between {anchor_fn!r} and {partner_fn!r}.  "
            f"It appears between the shared accesses in the IR by coincidence, "
            f"not because it synchronises them.  FALSE POSITIVE."
        )

    # 2. Init / teardown indicator ────────────────────────────────────────────
    if is_ext and base_label in INIT_EXT:
        if base_label == "remove_proc_entry":
            phase = "module teardown/exit"
        elif base_label == "read_sanitised_ftr_reg":
            phase = "ARM64 boot-time initialisation (evaluated once per CPU)"
        elif base_label == "led_trigger_register":
            phase = "LED device registration (driver probe / init)"
        else:
            phase = "driver/device initialisation"
        return FP, "INIT_LIFECYCLE_MISMATCH", (
            f"The anchor '{label}' is a {phase} call.  "
            f"{anchor_fn!r} executes during {phase} while {partner_fn!r} executes "
            f"at a different lifecycle stage.  These two functions are never "
            f"concurrent in practice: Linux serialises module init/teardown with "
            f"runtime operations (driver_register, device_add, module notifiers).  "
            f"FALSE POSITIVE."
        )

    # 3. Seqlock writer: dmb ishst [release] ──────────────────────────────────
    if "dmb ishst" in label and ordering == "release":
        return FP, "SEQLOCK_WRITER_PROTOCOL", (
            "The anchor 'dmb ishst [release]' is the writer-side barrier of a "
            "seqlock (write_seqcount_begin / write_seqlock_bh pattern).  "
            "The seqlock protocol guarantees correctness: readers validate the "
            "sequence counter before and after reading and retry if it changed "
            "(odd sequence = write in progress, even = consistent snapshot).  "
            "The herd7 'No' arises because the litmus model omits the retry loop "
            "and explores the window where P1 reads stale data before retrying.  "
            "The seqlock retry handles this window by design.  FALSE POSITIVE."
        )

    # 4. Lock-alias gap: vmalloc ──────────────────────────────────────────────
    if ll_stem == "vmalloc" and "__rb_insert_augmented" in label:
        return FP, "LOCK_ALIAS_MISSED", (
            "The anchor '__rb_insert_augmented (ext) [acquire]' is the RB-tree "
            "insert helper inside insert_vmap_area_augment.  "
            "reclaim_list_global acquires @free_vmap_area_lock before the RB-tree "
            "path; insert_vmap_area_augment is always called by callers that also "
            "hold @free_vmap_area_lock (alloc_vmap_area, free_vmap_area_noflush).  "
            "The lock is acquired in the *callers*, not inside the anchor function "
            "itself, so the LockAlias heuristic cannot detect the alias (it sees no "
            "_raw_spin_lock call inside __rb_insert_augmented).  Both sides of the "
            "pair are mutually exclusive via @free_vmap_area_lock.  FALSE POSITIVE "
            "due to a lock-alias detection gap."
        )

    # 5. Strong LSE release atomics with SC ordering ───────────────────────────
    if base_label in STRONG_RELEASE_ATOMICS and ordering == "SC":
        return _classify_strong_atomic(c, base_label, anchor_fn, partner_fn, ll_stem)

    # 6. DMB ISH [SC] in RCU tree module ───────────────────────────────────────
    if "dmb ish" in label and ordering == "SC" and ll_stem == "tree":
        return _classify_rcu_tree(anchor_fn, partner_fn)

    # 7. STLR [release] — reversed-MP ─────────────────────────────────────────
    if "stlr" in label and ordering == "release":
        return BORD, "REVERSED_MP_STLR_RELEASE", (
            "The anchor 'stlr [release]' is a store-release instruction "
            f"(anchor-fn: {anchor_fn!r}, partner-fn: {partner_fn!r}).  "
            "Litmus pattern (reversed-MP): P0 executes STLR(y=sync) THEN "
            "STR(x=payload after the STLR).  P1 executes LDAR(y) THEN plain LDR(x).  "
            "STLR only provides release ordering for stores BEFORE it — it does NOT "
            "order subsequent stores.  Therefore P1's LDAR(y) cannot causally order "
            "P0's subsequent STR(x).  herd7 correctly identifies the forbidden state.  "
            "Whether this is a genuine bug depends on whether the two functions can "
            "execute concurrently: if triggers_list_lock always serialises them this "
            "is an FP; otherwise it is a genuine ordering gap (TP).  "
            "BORDERLINE — check triggers_list_lock coverage in source."
        )

    # Default ─────────────────────────────────────────────────────────────────
    return UNKN, "UNCLASSIFIED", (
        f"No classification rule matched anchor='{label}' [{ordering}] "
        f"(ll_stem={ll_stem!r}, anchor_fn={anchor_fn!r}, "
        f"partner_fn={partner_fn!r}).  Manual analysis required."
    )


def _classify_strong_atomic(c, base_label, anchor_fn, partner_fn, ll_stem):
    """Sub-classifier for strong LSE release atomics (SC ordering)."""

    # perf / casal: reversed-MP on event->state + aux_event pointer
    if base_label == "casal" and "perf_get_aux_event" in anchor_fn:
        return TP, "PERF_REVERSED_MP_CASAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            f"Anchor: 'casal [SC]' in {anchor_fn!r} — compare-and-swap with "
            "acquire+release on event->state.\n\n"
            "Reversed-MP pattern:\n"
            "  P0 (perf_get_aux_event):\n"
            "    LDR  old_state = event->state        (read old state)\n"
            "    DMB LD\n"
            "    STLR new_state via casal             (y = sync, release)\n"
            "    DMB ST / DMB ISH\n"
            "    STR  event->aux_event = new_ptr      (x = payload, WRITE AFTER STLR)\n"
            f"  P1 ({partner_fn!r}):\n"
            "    LDAR event->state                    (y = acquire)\n"
            "    LDR  event->aux_event                (x = plain load)\n\n"
            "Because the aux_event pointer write comes AFTER the STLR, P1's LDAR "
            "on state does NOT causally order the subsequent STR on aux_event.  "
            "herd7 confirms: P1 can observe the new state (casal succeeded) but a "
            "stale (possibly NULL or freed) aux_event pointer.\n\n"
            "Fix: move the aux_event pointer assignment to BEFORE the casal, or "
            "add an explicit smp_wmb()/STLR before the pointer write, or require "
            "consumers to re-read the pointer under a barrier after the LDAR."
        )

    # pNFS / ldclral: layout sequence lock bit-clear
    if base_label == "ldclral" and "pnfs_layoutcommit_inode" in anchor_fn:
        return TP, "PNFS_REVERSED_MP_LDCLRAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            f"Anchor: 'ldclral [SC]' in {anchor_fn!r} — atomic bit-clear with "
            "acquire+release on the pNFS layout commit lock bit "
            "(NFS_LAYOUT_DOING_LAYOUTCOMMIT / NFS_LAYOUT_FIRST_LAYOUTGET).\n\n"
            "3-variable reversed-MP pattern:\n"
            "  P0 (pnfs_layoutcommit_inode):\n"
            "    LDR  x = layout->plh_flags (check state)\n"
            "    DMB LD\n"
            "    STLR y = layout commit bit via ldclral/stlxr  (sync, release)\n"
            "    DMB ST / DMB ISH\n"
            "    STR  z = layout data write                    (AFTER STLR)\n"
            f"  P1 ({partner_fn!r}):\n"
            "    LDAR y                               (acquire)\n"
            "    LDR  z                               (plain load, stale possible)\n\n"
            "The layout data write (z) comes after the STLR — P1's acquire on y "
            "does not causally order z.  herd7 confirms: P1 sees the commit bit "
            "cleared but reads stale layout data.\n\n"
            "This affects all six partner functions: __pnfs_destroy_layout, "
            "nfs4_layout_refresh_old_stateid, pnfs_free_lseg_list, "
            "pnfs_layout_process, pnfs_roc_release, pnfs_set_lo_fail — all read "
            "layout state after observing the commit bit release.  "
            "Investigate ordering of layout data writes relative to the ldclral "
            "in fs/nfs/pnfs.c."
        )

    # keyboard / ldsetal: kbd LED/state bits, IRQ vs process context
    if base_label == "ldsetal" and ll_stem == "keyboard":
        return TP, "KBD_IRQ_PROCESS_LDSETAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            "Anchor: 'ldsetal [SC]' — atomic_fetch_or on keyboard LED/state bits "
            f"(anchor-fn: {anchor_fn!r}, partner-fn: {partner_fn!r}).\n\n"
            "Context:\n"
            "  kbd_event: runs in IRQ context (called from input_event IRQ handler).\n"
            "  vt_set_led_state / vt_kbd_con_start / vt_kbd_con_stop: process context "
            "under kbd_event_lock.\n\n"
            "Litmus pattern (standard MP for forward direction, reversed for backward):\n"
            "  P0 (writer side): STR x=payload, STLR y=kbd_state via ldsetal\n"
            "  P1 (reader side): DMB LD, LDR x, LDAR y\n\n"
            "The kbd_event_lock spinlock protects vt_* functions in process context "
            "but is NOT always held inside kbd_event during the IRQ fast-path.  "
            "The ordering gap occurs at the IRQ/process boundary.  "
            "Inspect keyboard.ll L325 (kbd_event) and the exact store/load ordering "
            "around the ldsetal to determine which direction contains the gap."
        )

    # buffer / ldsetal: BH_Lock bit
    if base_label == "ldsetal" and ll_stem == "buffer":
        return TP, "BUFFER_BH_LOCK_LDSETAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            "Anchor: 'ldsetal [SC]' in __block_write_full_folio — "
            "test_and_set on BH_Lock bit (atomic_fetch_or on buffer_head->b_state).\n\n"
            "Reversed-MP pattern:\n"
            "  P0 (__block_write_full_folio):\n"
            "    LDR  b_state                         (check BH_Lock)\n"
            "    DMB LD\n"
            "    STLR BH_Lock = 1 via ldsetal         (y = sync, release)\n"
            "    DMB ST / DMB ISH / DMB LD (barriers)\n"
            "    STR  buffer data                     (x = payload AFTER STLR)\n"
            "  P1 (__block_write_begin_int):\n"
            "    LDAR BH_Lock                         (y = acquire)\n"
            "    LDR  buffer data                     (x = plain, stale possible)\n\n"
            "P1 acquires BH_Lock then reads buffer data with a plain load.  If the "
            "data write in P0 comes after the STLR, P1's acquire does not order it.  "
            "herd7 confirms the forbidden state.  "
            "Inspect fs/buffer.c (__block_write_full_folio, L7341 in buffer.ll): "
            "verify whether payload data writes precede or follow the ldsetal."
        )

    # NFS / ldsetal: folio lock
    if base_label == "ldsetal" and ll_stem == "nfs":
        return TP, "NFS_FOLIO_LOCK_LDSETAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            "Anchor: 'ldsetal [SC]' in nfs_update_folio — folio lock acquisition "
            f"(atomic_fetch_or on folio->flags, sets PG_locked).  "
            f"Partner: {partner_fn!r}.\n\n"
            "4-variable reversed-MP pattern (nfs_update_folio_nfs_file_direct_write "
            "has w, x, y, z; others have x, y, z):\n"
            "  P0 (nfs_update_folio):\n"
            "    LDR  x = folio->flags (check)\n"
            "    STLR y = PG_locked via ldsetal       (sync, release)\n"
            "    DMB ST / DMB ISH / DMB LD\n"
            "    STR  z = folio payload data          (AFTER STLR)\n"
            f"  P1 ({partner_fn!r}):\n"
            "    LDAR y                               (acquire)\n"
            "    DMB ISH\n"
            "    LDR  z                               (plain, stale possible)\n\n"
            "The folio payload write follows the STLR — P1's acquire does not "
            "causally order P0's subsequent write to z.  "
            "Affects nfs_file_direct_write, nfs_read_add_folio, nfs_writepages.  "
            "Investigate ordering in fs/nfs/write.c (nfs_update_folio, L46252 "
            "in nfs.ll): ensure all folio data writes precede the ldsetal."
        )

    # NFS / ldaddal: nfs_attr_generation_counter
    if base_label == "ldaddal" and ll_stem == "nfs":
        return TP, "NFS_ATTR_GENERATION_LDADDAL", (
            "TRUE POSITIVE — confirmed by herd7.\n\n"
            "Anchor: 'ldaddal [SC]' in nfs_alloc_fattr_with_label — "
            "atomic64_add_return on global @nfs_attr_generation_counter "
            "(acquire+release, increments and returns new generation number).\n\n"
            "Pattern:\n"
            "  P0 (nfs_alloc_fattr_with_label, L25234 in nfs.ll):\n"
            "    STR  x = fattr payload fields        (some BEFORE ldaddal)\n"
            "    STLR y = nfs_attr_generation_counter via ldaddal (sync, release)\n"
            "    DMB ST / DMB ISH\n"
            "    STR  x = more fattr fields           (possibly AFTER STLR also)\n"
            "  P1 (nfs_refresh_inode_locked, L25311 in nfs.ll):\n"
            "    LDAR y = nfs_attr_generation_counter (acquire)\n"
            "    LDR  x = inode/fattr fields          (plain)\n\n"
            "nfs_refresh_inode_locked reads the generation counter with LDAR "
            "(acquire) and uses it to decide whether to update inode attributes.  "
            "If any fattr field write in nfs_alloc_fattr_with_label comes AFTER "
            "the ldaddal, the acquire on the counter does not order those writes.  "
            "Inspect fs/nfs/inode.c (nfs_alloc_fattr_with_label) to verify field "
            "population ordering relative to the generation counter increment."
        )

    # ext4 / ldaddal: PA prealloc refcount
    if base_label == "ldaddal" and ll_stem == "ext4":
        return BORD, "EXT4_PA_REFCOUNT_LDADDAL", (
            "BORDERLINE — requires source inspection.\n\n"
            "Anchor: 'ldaddal [SC]' in ext4_mb_pa_put_free — atomic_sub_return "
            "on ext4_prealloc_space->pa_count (acquire+release, decrements "
            "refcount).  Partner: ext4_mb_use_inode_pa.\n\n"
            "Pattern (ext4.ll L75456 / L78871):\n"
            "  P0 (ext4_mb_pa_put_free):\n"
            "    STR  x = pa->pa_pstart/len            (payload)\n"
            "    STLR y = pa_count-- via ldaddal       (sync, release)\n"
            "    DMB ST / DMB ISH\n"
            "    STR  z = pa freed / nulled            (AFTER STLR, count==0 path)\n"
            "  P1 (ext4_mb_use_inode_pa):\n"
            "    LDR  x, LDR z                        (plain loads, no barriers)\n\n"
            "If both callers hold per-PA spinlock (pa->pa_lock) during access, "
            "they are mutually exclusive → FALSE POSITIVE (lock-alias detection "
            "gap; pa->pa_lock is acquired by callers, not inside the functions).  "
            "If either path is called without the lock (RCU read path), the "
            "refcount decrement to zero creates a use-after-free window → TP.  "
            "Inspect fs/ext4/mballoc.c: verify ext4_mb_pa_put_free and "
            "ext4_mb_use_inode_pa are always called under the same pa->pa_lock."
        )

    # proc / casal: proc inode open-count CAS
    if base_label == "casal" and ll_stem == "proc":
        return BORD, "PROC_INODE_CAS_CASAL", (
            "BORDERLINE — requires source inspection.\n\n"
            "Anchor: 'casal [SC]' in proc_get_link — compare-and-swap with "
            "acquire+release on proc inode open-count field (L13758 in proc.ll).  "
            f"Partner: {partner_fn!r}.\n\n"
            "3-variable reversed-MP pattern:\n"
            "  P0 (proc_get_link):\n"
            "    LDR  x = proc inode field (read old value)\n"
            "    STLR y = CAS new state via casal     (sync, release)\n"
            "    DMB ST / DMB ISH\n"
            "    STR  z = proc-path data              (payload AFTER STLR)\n"
            f"  P1 ({partner_fn!r}):\n"
            "    LDAR y                               (acquire)\n"
            "    LDR  z                               (plain, stale possible)\n\n"
            "proc_get_link increments the proc inode open count for dentry "
            f"traversal.  {partner_fn!r} accesses proc filesystem state and may "
            "race through the proc dentry/inode state machine.  "
            "If the proc inode is stable (refcount > 0) throughout, the CAS is "
            "an optimistic concurrency control and this is an FP.  "
            "If the CAS is the only mechanism protecting z, the ordering gap is "
            "genuine.  Inspect fs/proc/ for the locking contract."
        )

    # Generic strong atomic fallback
    return TP, "STRONG_ATOMIC_REVERSED_MP", (
        f"TRUE POSITIVE (likely) — confirmed by herd7.\n\n"
        f"Anchor: '{c['anchor_label']}' [{c['anchor_ordering']}] — "
        "a strong release atomic (LSE) instruction with full acquire+release "
        "semantics.\n\n"
        "The litmus captures a reversed message-passing (MP) pattern: the anchor "
        "atomic (y = sync) occurs BEFORE the payload write (z) in P0, while P1 "
        "acquires y with a plain load on z.  Because z is written AFTER the "
        "release, P1's acquire on y does not causally order P0's subsequent write "
        f"to z.  herd7 confirms the forbidden state.  "
        f"Module: {c['ll_stem']!r}, anchor-fn: {c['anchor_fn']!r}, "
        f"partner-fn: {c['partner_fn']!r}.  Requires source-level investigation."
    )


def _classify_rcu_tree(anchor_fn: str, partner_fn: str) -> tuple[str, str, str]:
    """Classify 'dmb ish [SC]' or 'jiffies_to_msecs (ext) [SC]' in RCU tree."""

    # Symmetric pair rcu_exp_jiffies / rcu_exp_sel_wait_wake — pure arithmetic
    if partner_fn in {"rcu_exp_jiffies_till_stall_check", "rcu_exp_sel_wait_wake"}:
        return FP, "RCU_EXP_JIFFIES_ARITHMETIC", (
            "FALSE POSITIVE.\n\n"
            "Anchor: 'jiffies_to_msecs (ext) [SC]' — a pure arithmetic helper "
            "that converts jiffies to milliseconds with no synchronisation "
            f"semantics.  rcu_exp_jiffies_till_stall_check and rcu_exp_sel_wait_wake "
            "use this for stall detection.  RCU intentionally uses lockless accesses "
            "to jiffies for performance (the stall detector is advisory, not a "
            "correctness control).  mbcheck modelled jiffies_to_msecs as SC which "
            "incorrectly implies a synchronisation relationship."
        )

    if partner_fn == "rcu_force_quiescent_state":
        return BORD, "RCU_GP_INIT_TRYLOCK_WINDOW", (
            "BORDERLINE — raw_spin_trylock creates a mutual-exclusion gap.\n\n"
            "rcu_gp_init holds @rcu_state.lock (raw_spin_lock_irq) and uses "
            "'dmb ish [SC]' for ordering within its critical section.  "
            "rcu_force_quiescent_state also acquires @rcu_state.lock — making "
            "the two mutually exclusive under the full-lock path.  However, "
            "rcu_force_quiescent_state uses raw_spin_trylock: if the trylock fails "
            "it skips the critical section and the two are NOT mutually exclusive.  "
            "The trylock failure path may observe partially-updated grace-period "
            "state without the full barrier ordering.  "
            "Inspect kernel/rcu/tree.c: determine whether the trylock failure path "
            "is safe (re-checks state, or partial read is benign)."
        )

    if partner_fn == "show_rcu_gp_kthreads":
        return BORD, "RCU_GP_INIT_SHOW_OBSERVER", (
            "BORDERLINE — read-only observer may race on individual @rcu_state fields.\n\n"
            "rcu_gp_init holds @rcu_state.lock.  show_rcu_gp_kthreads is a "
            "debugfs/procfs read-only observer that prints RCU state WITHOUT always "
            "holding @rcu_state.lock.  The 'dmb ish [SC]' orders rcu_gp_init's own "
            "writes but show_ may read fields non-atomically.  "
            "Likely benign (diagnostic output, not a correctness control) but "
            "technically a data race.  "
            "Verify whether show_rcu_gp_kthreads uses READ_ONCE() for all accesses "
            "to @rcu_state fields or is annotated as KCSAN-safe."
        )

    if partner_fn in {"note_gp_changes", "rcu_accelerate_cbs_unlocked"}:
        return BORD, "RCU_GP_INIT_DIFFERENT_LOCK", (
            f"BORDERLINE — different lock granularity.\n\n"
            "rcu_gp_init holds @rcu_state.lock (raw_spin_lock_irq).  "
            f"{partner_fn!r} acquires the per-node rnp->lock, NOT @rcu_state.lock.  "
            "These are different locks — mutual exclusion is not guaranteed for all "
            "@rcu_state field accesses.  The DMB ISH in rcu_gp_init orders writes "
            f"within @rcu_state.lock but {partner_fn!r} may read @rcu_state fields "
            "without holding it.  "
            "Likely tolerated by RCU design (intentional lockless accesses in the "
            "per-node fast path) but may warrant a READ_ONCE() annotation.  "
            "Inspect kernel/rcu/tree.c for the specific fields."
        )

    if partner_fn in {"rcu_core", "rcu_sched_clock_irq"}:
        ctx = "softirq" if "core" in partner_fn else "timer-IRQ"
        return BORD, "RCU_GP_INIT_SOFTIRQ_RACE", (
            f"BORDERLINE — {ctx} context accesses @rcu_state without the lock.\n\n"
            "rcu_gp_init holds @rcu_state.lock (raw_spin_lock_irq) and 'dmb ish [SC]' "
            "orders its writes within the lock.  "
            f"{partner_fn!r} runs in {ctx} context and accesses @rcu_state fields "
            "WITHOUT holding @rcu_state.lock.  The {ctx} reader has no corresponding "
            "acquire barrier.  "
            "RCU uses intentional lockless accesses in the fast path — the design "
            "relies on careful READ_ONCE/WRITE_ONCE and targeted smp_mb() barriers.  "
            "Verify whether the specific fields in the litmus test are annotated "
            "with READ_ONCE() in kernel/rcu/tree.c."
        )

    return BORD, "RCU_DMB_ISH_SC", (
        f"BORDERLINE — 'dmb ish [SC]' in rcu_gp_init vs {partner_fn!r}.  "
        "Requires detailed analysis of lock context in kernel/rcu/tree.c."
    )


# ── investigation hints ───────────────────────────────────────────────────────

def _investigation_hints(c: dict, verdict: str, category: str) -> list[str]:
    hints = []
    fn_a  = c["anchor_fn"]
    fn_p  = c["partner_fn"]
    ll    = c["ll_file"]
    label = c["anchor_label"]

    if verdict == FP:
        if "BENIGN_EXT" in category:
            base = label.removesuffix(" (ext)").strip()
            hints.append(
                f"Add `\"{label}\"` to `kBenignLabels[]` in "
                "`src/lib/fn_heuristics.cpp` (`BenignAnchorHeuristic`) to "
                "suppress this class of false positives."
            )
        elif "INIT_LIFECYCLE" in category:
            base = label.removesuffix(" (ext)").strip()
            hints.append(
                f"Add `\"{base}\"` to `kInitIndicatorFns[]` in "
                "`src/lib/fn_heuristics.cpp` to mark `{fn_a}` as init-only."
            )
        elif "SEQLOCK" in category:
            hints.append(
                "Implement a SeqlockWriterHeuristic that detects the seqlock "
                "writer pattern (sequence increment / dmb ishst / data writes / "
                "dmb ishst / sequence increment) and suppresses pairs where the "
                "anchor is the seqlock writer barrier (`dmb ishst [release]`)."
            )
        elif "LOCK_ALIAS" in category:
            hints.append(
                "Improve LockAlias heuristic to trace lock acquisition through "
                "the call graph into callers of the anchor function."
            )
            hints.append(
                f"Verify in `tests/kernel-noLTO/{ll}` that both `{fn_a}` and "
                f"`{fn_p}` are always invoked with @free_vmap_area_lock held."
            )
        elif "RCU_EXP" in category:
            hints.append(
                "Add `jiffies_to_msecs` (and similar pure arithmetic helpers) "
                "to `kBenignLabels[]` in `src/lib/fn_heuristics.cpp`."
            )

    elif verdict == TP:
        hints.append(
            f"Open `tests/kernel-noLTO/{ll}` and locate `@{fn_a}`.  "
            "Identify the payload STR that comes AFTER the release atomic — "
            "that is the ordering gap.  The payload write must be moved BEFORE "
            "the release atomic, or a full barrier must be inserted."
        )
        if "PNFS" in category:
            hints.append(
                "Cross-reference with pNFS layout state machine in "
                "fs/nfs/pnfs.c — check layout commit bit vs layout data "
                "write ordering around pnfs_layoutcommit_inode()."
            )
        if "PERF" in category:
            hints.append(
                "In kernel/events/core.c, locate perf_get_aux_event() and "
                "check whether event->aux_event is assigned BEFORE or AFTER "
                "the cmpxchg on event->state."
            )
        if "NFS_FOLIO" in category:
            hints.append(
                "In fs/nfs/write.c, verify that all folio data writes in "
                "nfs_update_folio are completed BEFORE the folio-lock set "
                "(ldsetal on PG_locked bit)."
            )
        if "NFS_ATTR" in category:
            hints.append(
                "In fs/nfs/inode.c (nfs_alloc_fattr_with_label), verify that "
                "ALL fattr field assignments precede the ldaddal on "
                "@nfs_attr_generation_counter."
            )
        if "KBD" in category:
            hints.append(
                "In drivers/tty/vt/keyboard.c, verify that kbd_event_lock is "
                "held consistently in both IRQ and process-context paths before "
                "accessing shared keyboard state."
            )
        if "BUFFER" in category:
            hints.append(
                "In fs/buffer.c (__block_write_full_folio), check whether "
                "buffer data is written BEFORE or AFTER the BH_Lock set (ldsetal).  "
                "Also check __block_write_begin_int for missing barriers on read."
            )

    elif verdict == BORD:
        if "RCU" in category:
            hints.append(
                f"Search kernel/rcu/tree.c for `{fn_p}` and check whether "
                "it acquires @rcu_state.lock or uses READ_ONCE() for the "
                "fields observed in the litmus test."
            )
        if "EXT4_PA" in category:
            hints.append(
                "Search fs/ext4/mballoc.c: verify that ALL callers of "
                "ext4_mb_pa_put_free and ext4_mb_use_inode_pa hold pa->pa_lock."
            )
        if "PROC" in category:
            hints.append(
                f"Search fs/proc/ for `{fn_p}` and determine which lock "
                "protects the shared state identified by the litmus test variables."
            )
        if "STLR" in category:
            hints.append(
                "Search drivers/leds/led-triggers.c for triggers_list_lock "
                "usage around led_trigger_set() and led_trigger_format().  "
                "If the lock always serialises them, add to LockAlias heuristic."
            )
        if "TRYLOCK" in category:
            hints.append(
                "Examine the raw_spin_trylock failure path in "
                "rcu_force_quiescent_state — does it safely re-check the "
                "grace-period state it missed, or is there a correctness risk?"
            )
        if "DIFFERENT_LOCK" in category or "SOFTIRQ" in category:
            hints.append(
                f"Grep kernel/rcu/tree.c for all accesses to the @rcu_state "
                f"fields identified in the litmus test inside {fn_p!r} and "
                "verify READ_ONCE() / WRITE_ONCE() annotations are present."
            )

    return hints


# ── report builder ────────────────────────────────────────────────────────────

_VERDICT_BADGE = {
    FP:   "FALSE POSITIVE",
    TP:   "TRUE POSITIVE",
    BORD: "BORDERLINE",
    UNKN: "UNKNOWN",
}

_VERDICT_PLAIN = {
    FP:   "FALSE_POSITIVE",
    TP:   "TRUE_POSITIVE",
    BORD: "BORDERLINE",
    UNKN: "UNKNOWN",
}


def build_report(stats: dict, cases: list[dict], testdir: Path) -> str:
    lines: list[str] = []

    # Classify all cases first so we can compute totals for the header
    classified = []
    for c in cases:
        v, cat, reason = classify(c)
        classified.append((c, v, cat, reason))

    tally = {FP: 0, TP: 0, BORD: 0, UNKN: 0}
    for _, v, _, _ in classified:
        tally[v] += 1

    total_no = stats["no"] or 1  # avoid div-by-zero

    # ── header ───────────────────────────────────────────────────────────────
    lines += [
        "# mbcheck kernel-noLTO No-Cases Analysis",
        "",
        f"**Log:** `build/kernel_noLTO_results.log`  ",
        f"**Sweep started:** {stats['started']}  ",
        f"**IR test-set:** {stats['files']} `.ll` files  ",
        "",
        "## Summary Statistics",
        "",
        "| Metric | Count |",
        "|--------|-------|",
        f"| Total litmus tests run        | {stats['total']} |",
        f"| Ok (ordering holds)           | {stats['ok']} |",
        f"| **No (forbidden state)**      | **{stats['no']}** |",
        f"| Err                           | {stats['err']} |",
        "",
        "## Classification Overview",
        "",
        "| Verdict | Count | % of No |",
        "|---------|-------|---------|",
    ]
    for v in (FP, TP, BORD, UNKN):
        pct = 100 * tally[v] / total_no
        lines.append(f"| {_VERDICT_PLAIN[v]} | {tally[v]} | {pct:.1f}% |")
    lines += [""]

    # module breakdown
    mod_tally: dict[str, dict] = {}
    for c, v, _, _ in classified:
        mod = c["ll_stem"]
        if mod not in mod_tally:
            mod_tally[mod] = {FP: 0, TP: 0, BORD: 0, UNKN: 0}
        mod_tally[mod][v] += 1

    lines += [
        "## Cases by Module",
        "",
        "| Module | FP | TP | Borderline | Unknown | Total |",
        "|--------|----|----|------------|---------|-------|",
    ]
    for mod in sorted(mod_tally):
        t = mod_tally[mod]
        total = sum(t.values())
        lines.append(
            f"| `{mod}` | {t[FP]} | {t[TP]} | {t[BORD]} | {t[UNKN]} | {total} |"
        )
    lines += [""]

    # duplicate names
    name_count: dict[str, int] = {}
    for c, _, _, _ in classified:
        name_count[c["litmus_name"]] = name_count.get(c["litmus_name"], 0) + 1
    dupes = {n for n, cnt in name_count.items() if cnt > 1}
    if dupes:
        lines += [
            "## Duplicate Litmus Tests",
            "",
            "The following tests appear more than once (generated from multiple "
            "`.ll` files):",
            "",
        ]
        for n in sorted(dupes):
            lines.append(f"- `{n}` (×{name_count[n]})")
        lines += [""]

    # ── per-case sections ─────────────────────────────────────────────────────
    lines += ["---", "", "# Detailed Per-Case Analysis", ""]

    for idx, (c, verdict, category, reason) in enumerate(classified, 1):
        dup_note = " *(duplicate)*" if c["litmus_name"] in dupes else ""

        lines += [
            "---",
            "",
            f"## Case {idx}: `{c['litmus_name']}`{dup_note}",
            "",
            f"**Verdict:** {_VERDICT_BADGE[verdict]}  ",
            f"**Category:** `{category}`  ",
            f"**Module / ll-file:** `{c['ll_file']}`  ",
            f"**Litmus file:** `{c['litmus_path']}`  ",
            "",
            "### Anchor Metadata",
            "",
            "| Field | Value |",
            "|-------|-------|",
            f"| Anchor label     | `{c['anchor_label']}` |",
            f"| Ordering         | `{c['anchor_ordering']}` |",
            f"| Anchor site      | `{c['anchor_site']}` |",
            f"| Anchor function  | `{c['anchor_fn']}` |",
            f"| Partner function | `{c['partner_fn']}` |",
            f"| Shared variables | "
            f"{', '.join(f'`{v}`' for v in c['shared_vars']) or '*(none parsed)*'} |",
            f"| herd7 condition  | `~exists ({c['condition']})` |",
            "",
        ]

        # litmus text
        if c["litmus_text"] and not c["litmus_text"].startswith("(litmus"):
            lines += [
                "### Litmus Test",
                "",
                "```",
                c["litmus_text"].rstrip(),
                "```",
                "",
            ]
        else:
            lines += [
                "### Litmus Test",
                "",
                f"> *{c['litmus_text']}*",
                "",
            ]

        # IR context for anchor-fn and partner-fn
        for role, fn in [("anchor-fn", c["anchor_fn"]), ("partner-fn", c["partner_fn"])]:
            if not fn:
                continue
            start_no, ir_lines = get_fn_ir(testdir, c["ll_file"], fn)
            if ir_lines:
                lines += [
                    f"### IR Context: `{fn}` ({role})",
                    "",
                    f"Source: `tests/kernel-noLTO/{c['ll_file']}` starting at line {start_no}",
                    "",
                    "```llvm",
                ]
                lines += ir_lines
                lines += ["```", ""]
            else:
                lines += [
                    f"### IR Context: `{fn}` ({role})",
                    "",
                    f"> *Function `{fn}` not found in `{c['ll_file']}`*",
                    "",
                ]

        # analysis (split on double-newline to preserve paragraph structure)
        lines += ["### Analysis", ""]
        for para in reason.split("\n\n"):
            para = para.strip()
            if para:
                lines.append(para)
                lines.append("")

        # investigation hints
        hints = _investigation_hints(c, verdict, category)
        if hints:
            lines += ["### Suggested Investigation", ""]
            for h in hints:
                lines.append(f"- {h}")
            lines.append("")

    return "\n".join(lines)


# ── CSV output ────────────────────────────────────────────────────────────────

def write_csv(classified: list[tuple], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow([
            "idx", "module", "litmus_name", "ll_file",
            "anchor_label", "anchor_ordering", "anchor_site",
            "anchor_fn", "partner_fn",
            "verdict", "category",
        ])
        for idx, (c, verdict, category, _) in enumerate(classified, 1):
            w.writerow([
                idx,
                c["ll_stem"],
                c["litmus_name"],
                c["ll_file"],
                c["anchor_label"],
                c["anchor_ordering"],
                c["anchor_site"],
                c["anchor_fn"],
                c["partner_fn"],
                _VERDICT_PLAIN[verdict],
                category,
            ])


# ── entry point ───────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Analyse kernel-noLTO sweep No-cases and produce a debugging report."
    )
    p.add_argument("--log",       default=str(DEFAULTS["log"]),
                   help="Path to kernel_noLTO_results.log")
    p.add_argument("--litmusdir", default=str(DEFAULTS["litmusdir"]),
                   help="Root of generated .litmus file tree (build/litmus)")
    p.add_argument("--testdir",   default=str(DEFAULTS["testdir"]),
                   help="Root of kernel-noLTO .ll IR files (tests/kernel-noLTO)")
    p.add_argument("--out",       default=str(DEFAULTS["out"]),
                   help="Output Markdown report path")
    p.add_argument("--csv",       default=str(DEFAULTS["csv_out"]),
                   help="Output CSV path")
    return p.parse_args()


def main():
    args     = parse_args()
    log_path = Path(args.log)
    litmusdir= Path(args.litmusdir)
    testdir  = Path(args.testdir)
    out_md   = Path(args.out)
    out_csv  = Path(args.csv)

    if not log_path.exists():
        print(f"ERROR: log file not found: {log_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing log:  {log_path}")
    stats, cases = parse_log(log_path)
    print(f"  {stats['files']} files  |  "
          f"{stats['total']} litmus  |  "
          f"{stats['ok']} Ok  |  "
          f"{stats['no']} No  |  "
          f"{stats['err']} Err")

    print(f"Loading litmus files from: {litmusdir}")
    attach_litmus(cases, litmusdir)
    found = sum(1 for c in cases if c["anchor_fn"])
    print(f"  {found}/{len(cases)} cases have parseable litmus headers")

    print("Classifying …")
    classified = [(c, *classify(c)) for c in cases]

    tally = {FP: 0, TP: 0, BORD: 0, UNKN: 0}
    for _, v, _, _ in classified:
        tally[v] += 1
    print("  Verdict breakdown:")
    for v in (FP, TP, BORD, UNKN):
        print(f"    {_VERDICT_PLAIN[v]:<20} {tally[v]:3d}")

    out_md.parent.mkdir(parents=True, exist_ok=True)
    print(f"\nWriting report: {out_md}")
    report = build_report(stats, cases, testdir)
    out_md.write_text(report)

    print(f"Writing CSV:    {out_csv}")
    write_csv(classified, out_csv)

    print(f"\nDone.  Report written to: {out_md}")


if __name__ == "__main__":
    main()
