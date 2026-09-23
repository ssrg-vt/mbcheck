#!/usr/bin/env bash
# gen_kernel_ir.sh — Merge LLVM IR bitcode objects for every module directory
# in a ThinLTO-compiled Linux kernel tree and emit readable .ll files.
#
# Usage:
#   ./gen_kernel_ir.sh [OPTIONS]
#
# Options:
#   -k <path>   Linux kernel source root (required, or set $LINUX)
#   -o <path>   Output root (default: <workspace>/tests/kernel)
#   -j <n>      Parallel jobs (default: nproc)
#   -v          Verbose: print each directory as it is processed
#   -n          Dry-run: print commands without executing
#
# Output layout mirrors the kernel source tree:
#   <outroot>/drivers/net/ethernet/intel/e1000e/merged.ll
#   <outroot>/kernel/merged.ll
#   ...
#
# Each merged.ll is produced by:
#   llvm-link-18 <dir>/*.o | llvm-dis-18 > <outdir>/merged.ll
#
# Directories that contain only a single .o get a direct llvm-dis (no link step).
# Directories with no .o files are skipped.
#
# A log is written to <outroot>/gen_kernel_ir.log

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
LINUX="${LINUX:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTROOT="${OUTROOT:-$WORKSPACE/tests/kernel}"
JOBS="${JOBS:-$(nproc)}"
VERBOSE=0
DRY_RUN=0

LLVM_LINK="${LLVM_LINK:-llvm-link-18}"
LLVM_DIS="${LLVM_DIS:-llvm-dis-18}"

# ── Argument parsing ─────────────────────────────────────────────────────────
while getopts "k:o:j:vn" opt; do
    case "$opt" in
        k) LINUX="$OPTARG" ;;
        o) OUTROOT="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        v) VERBOSE=1 ;;
        n) DRY_RUN=1 ;;
        *) echo "Unknown option: $opt"; exit 1 ;;
    esac
done

# ── Validate tools ───────────────────────────────────────────────────────────
for tool in "$LLVM_LINK" "$LLVM_DIS"; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: $tool not found in PATH" >&2
        exit 1
    fi
done

if [[ ! -d "$LINUX" ]]; then
    echo "ERROR: Linux source not found: $LINUX" >&2
    exit 1
fi

mkdir -p "$OUTROOT"
LOG="$OUTROOT/gen_kernel_ir.log"
> "$LOG"

echo "=== gen_kernel_ir.sh ===" | tee -a "$LOG"
echo "  Linux source : $LINUX" | tee -a "$LOG"
echo "  Output root  : $OUTROOT" | tee -a "$LOG"
echo "  Parallel jobs: $JOBS" | tee -a "$LOG"
echo "  Started      : $(date)" | tee -a "$LOG"
echo "" | tee -a "$LOG"

# ── Worker function (runs in a sub-shell via xargs/parallel) ─────────────────
# Arguments: <linux_root> <outroot> <dir> <verbose> <dry_run>
process_dir() {
    local linux_root="$1"
    local outroot="$2"
    local dir="$3"
    local verbose="$4"
    local dry_run="$5"
    local llvm_link="${6:-llvm-link-18}"
    local llvm_dis="${7:-llvm-dis-18}"

    # Relative path from linux root, e.g. "drivers/net/ethernet/intel/e1000e"
    # Special-case the root directory itself (vmlinux.o etc.)
    local rel
    if [[ "$dir" == "$linux_root" ]]; then
        rel="_root_"
    else
        rel="${dir#"$linux_root/"}"
    fi
    local outdir="$outroot/$rel"
    local outfile="$outdir/merged.ll"

    # Collect LLVM IR bitcode .o files in this directory (non-recursive).
    # Exclude *.mod.o (kernel module metadata — each defines __this_module,
    # causing multiply-defined symbol errors when merged across modules).
    # Exclude *.nvhe.o (KVM nVHE hypervisor variant builds — same sources
    # compiled twice, causing duplicate symbol errors, e.g. kvm_condition_valid32).
    # Also exclude ELF/raw-binary .o files compiled from assembly sources.
    local objs=()
    while IFS= read -r -d '' f; do
        # Skip module metadata and nVHE variant files
        [[ "$f" == *.mod.o ]] && continue
        [[ "$f" == *.nvhe.o ]] && continue
        # Check LLVM bitcode magic: first 2 bytes = "BC" (0x42 0x43)
        if [[ "$(head -c2 "$f" 2>/dev/null)" == "BC" ]]; then
            objs+=("$f")
        fi
    done < <(find "$dir" -maxdepth 1 -name "*.o" -print0 | sort -z)

    if [[ ${#objs[@]} -eq 0 ]]; then
        return 0
    fi

    [[ "$verbose" == "1" ]] && echo "[  ] $rel (${#objs[@]} objects)"

    if [[ "$dry_run" == "1" ]]; then
        echo "WOULD: mkdir -p $outdir && llvm-link-18 ${objs[*]} | llvm-dis-18 > $outfile"
        return 0
    fi

    mkdir -p "$outdir"

    local rc=0
    if [[ ${#objs[@]} -eq 1 ]]; then
        # Single object — just disassemble
        "$llvm_dis" "${objs[0]}" -o "$outfile" 2>>"$outdir/stderr.log" || rc=$?
    else
        # Multiple objects — rename per-module symbols (init_module, cleanup_module)
        # that are defined once per kernel module to avoid multiply-defined errors,
        # then link all objects and disassemble.
        local link_objs=()
        local tmp_bcs=()
        for i in "${!objs[@]}"; do
            if (( i == 0 )); then
                link_objs+=("${objs[$i]}")
            else
                local tmp_bc
                tmp_bc=$(mktemp "/tmp/mbcheck_bc_XXXXXX.bc")
                tmp_bcs+=("$tmp_bc")
                # Disassemble → rename module entry-point symbols (and matching
                # ThinLTO summary entries) → reassemble.
                # We rename both the @-reference and the name: "..." in the
                # ThinLTO summary to keep them consistent; otherwise llvm-as
                # reports "Reference to undefined global".
                if llvm-dis-18 "${objs[$i]}" -o - 2>/dev/null \
                    | sed "s/@init_module\b/@init_module_${i}/g;
                           s/@cleanup_module\b/@cleanup_module_${i}/g;
                           s/name: \"init_module\"/name: \"init_module_${i}\"/g;
                           s/name: \"cleanup_module\"/name: \"cleanup_module_${i}\"/g" \
                    | llvm-as-18 -o "$tmp_bc" 2>/dev/null; then
                    link_objs+=("$tmp_bc")
                else
                    link_objs+=("${objs[$i]}")   # fallback: original (may conflict)
                fi
            fi
        done

        "$llvm_link" "${link_objs[@]}" 2>>"$outdir/stderr.log" \
            | "$llvm_dis" -o "$outfile" 2>>"$outdir/stderr.log" || rc=$?

        # Cleanup temp bitcode files
        for tmp_bc in "${tmp_bcs[@]}"; do
            rm -f "$tmp_bc"
        done
    fi

    if [[ $rc -ne 0 ]]; then
        echo "FAIL: $rel (exit $rc)" >&2
        echo "FAIL: $rel (exit $rc)" >> "$outdir/stderr.log"
        return 0   # don't abort the whole job
    fi

    local size
    size=$(wc -c < "$outfile")
    [[ "$verbose" == "1" ]] && echo "[OK] $rel → $outfile ($size bytes)"
    echo "OK: $rel" >> "$outdir/stderr.log"
}

export -f process_dir

# ── Discover all directories that have .o files ──────────────────────────────
echo "Scanning for .o files under $LINUX ..." | tee -a "$LOG"
DIRS_FILE=$(mktemp)
find "$LINUX" \
    -not -path "*/.git/*" \
    -name "*.o" \
    -printf "%h\n" \
    2>/dev/null \
    | sort -u > "$DIRS_FILE"

TOTAL=$(wc -l < "$DIRS_FILE")
echo "Found $TOTAL directories with .o files." | tee -a "$LOG"
echo "" | tee -a "$LOG"

if [[ $TOTAL -eq 0 ]]; then
    echo "No .o files found. Exiting." | tee -a "$LOG"
    rm "$DIRS_FILE"
    exit 0
fi

# ── Run in parallel via xargs ─────────────────────────────────────────────────
# Pass all args as env vars so the exported function can use them
export LINUX OUTROOT VERBOSE DRY_RUN LLVM_LINK LLVM_DIS

cat "$DIRS_FILE" \
    | xargs -d '\n' -P "$JOBS" -I{} \
        bash -c 'process_dir "$LINUX" "$OUTROOT" "$1" "$VERBOSE" "$DRY_RUN" "$LLVM_LINK" "$LLVM_DIS"' \
        _ {}

rm "$DIRS_FILE"

# ── Summary ───────────────────────────────────────────────────────────────────
OK_COUNT=$(find "$OUTROOT" -name "merged.ll" 2>/dev/null | wc -l)
FAIL_COUNT=$(grep -rl "^FAIL:" "$OUTROOT" 2>/dev/null | wc -l)

echo "" | tee -a "$LOG"
echo "=== Done ===" | tee -a "$LOG"
echo "  Finished     : $(date)" | tee -a "$LOG"
echo "  Merged files : $OK_COUNT" | tee -a "$LOG"
echo "  Failures     : $FAIL_COUNT" | tee -a "$LOG"
echo "  Output root  : $OUTROOT" | tee -a "$LOG"

if [[ "$FAIL_COUNT" -gt 0 ]]; then
    echo "" | tee -a "$LOG"
    echo "Directories with errors:" | tee -a "$LOG"
    grep -rl "^FAIL:" "$OUTROOT" 2>/dev/null \
        | sed "s|$OUTROOT/||; s|/stderr.log||" \
        | tee -a "$LOG"
fi
