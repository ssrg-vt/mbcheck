#!/usr/bin/env bash
# Reproduce one claim.
#
#   scripts/run.sh <claim> [--force] [--jobs N] [--timeout SEC] [--list]
#
#   smoke       50 modules, ~1 min
#   table4      kernel sweep, v6.19
#   table5      runtime breakdown
#   table6      top anchors and external functions
#   table7      heuristic hits
#   table8      microbenchmark suite
#   table9      violations by module, v6.19
#   all         all of the above
#
# Tables 4-9 share one cached sweep; --force re-runs it.
#
# Needs MBCHECK_CORPUS_V619, and KDIR for table8.  The corpus is not shipped;
# build one with scripts/build-kernel-ir.sh.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS="${MBCHECK_RESULTS:-$REPO/results}"
MBCHECK="${MBCHECK:-$REPO/build/mbcheck}"
CORPUS_V619="${MBCHECK_CORPUS_V619:-$REPO/tests/kernel-noLTO-v6.19}"
JOBS="$(nproc)"
TIMEOUT="${MBCHECK_TIMEOUT:-120}"
FORCE=0

CLAIMS=(smoke table4 table5 table6 table7 table8 table9)

usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; }

die() { echo "error: $*" >&2; exit 1; }

CLAIM=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force)  FORCE=1; shift ;;
        --jobs)   JOBS="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --list)   printf '%s\n' "${CLAIMS[@]}" all; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        -*)       die "unknown option: $1" ;;
        *)        CLAIM="$1"; shift ;;
    esac
done
[[ -n "$CLAIM" ]] || { usage; exit 1; }

mkdir -p "$RESULTS"

require_mbcheck() {
    [[ -x "$MBCHECK" ]] || die "mbcheck not built: $MBCHECK
  Build it first, or set \$MBCHECK."
}

require_corpus() {
    local dir="$1" name="$2" var="$3"
    [[ -d "$dir" ]] || die "$name IR corpus not found: $dir
  The corpus does not ship in this repository.  Build it with
    scripts/build-kernel-ir.sh <kernel-source> <output-dir>
  then export $var=<output-dir>.  See README.md."
    find "$dir" -name '*.ll' -print -quit | grep -q . \
        || die "$name IR corpus contains no .ll files: $dir"
}

ensure_sweep() {
    local tag="$1" corpus="$2" name="$3"
    local json="$RESULTS/sweep-$tag.json"

    if [[ -f "$json" && $FORCE -eq 0 ]]; then
        echo "  using cached sweep: $json"
        echo "  (pass --force to re-run)"
        return 0
    fi

    require_mbcheck
    require_corpus "$corpus" "$name" "MBCHECK_CORPUS_${tag^^}"

    echo "  sweeping $name ($(find "$corpus" -name '*.ll' | wc -l) modules, $JOBS workers)"
    echo "  this takes several minutes; progress follows"
    echo
    python3 "$REPO/src/script/run_kernel_noLTO.py" \
        --mbcheck   "$MBCHECK" \
        --testdir   "$corpus" \
        --jobs      "$JOBS" \
        --timeout   "$TIMEOUT" \
        --log       "$RESULTS/sweep-$tag.log" \
        --litmusdir "$RESULTS/litmus-$tag" \
        --json      "$json" \
        > "$RESULTS/sweep-$tag.stdout" 2>&1 \
        || { tail -20 "$RESULTS/sweep-$tag.stdout" >&2
             die "sweep failed; see $RESULTS/sweep-$tag.stdout"; }
    echo "  sweep complete"
}

report() {
    python3 "$REPO/scripts/report.py" "$1" "$2"
}

claim_smoke() {
    require_mbcheck
    require_corpus "$CORPUS_V619" "v6.19" MBCHECK_CORPUS_V619
    echo "  50 modules of the v6.19 corpus"
    python3 "$REPO/src/script/run_kernel_noLTO.py" \
        --mbcheck "$MBCHECK" --testdir "$CORPUS_V619" --jobs "$JOBS" --limit 50 \
        --log "$RESULTS/smoke.log" --litmusdir "$RESULTS/litmus-smoke" \
        --json "$RESULTS/smoke.json" > "$RESULTS/smoke.stdout" 2>&1 \
        || { tail -20 "$RESULTS/smoke.stdout" >&2; die "smoke run failed"; }
    report smoke "$RESULTS/smoke.json"
}

claim_table4() { ensure_sweep v619 "$CORPUS_V619" "v6.19"; report table4 "$RESULTS/sweep-v619.json"; }
claim_table6() { ensure_sweep v619 "$CORPUS_V619" "v6.19"; report table6 "$RESULTS/sweep-v619.json"; }
claim_table7() { ensure_sweep v619 "$CORPUS_V619" "v6.19"; report table7 "$RESULTS/sweep-v619.json"; }
claim_table9() { ensure_sweep v619 "$CORPUS_V619" "v6.19"; report table9 "$RESULTS/sweep-v619.json"; }
claim_table5() { ensure_sweep v619 "$CORPUS_V619" "v6.19"; report table5 "$RESULTS/sweep-v619.json"; }

claim_table8() {
    require_mbcheck
    local irdir="$RESULTS/sync-ir"
    if [[ ! -d "$irdir" || $FORCE -eq 1 ]]; then
        [[ -n "${KDIR:-}" ]] || die "KDIR is not set.
  The suite is compiled with the kernel's own flags, so it needs a built
  kernel tree:  export KDIR=<kernel-source>.  See README.md."
        echo "  compiling the suite against \$KDIR=$KDIR"
        "$REPO/scripts/build-testsuite-ir.sh" "$KDIR" \
            "$REPO/tests/kernel-sync" "$irdir"
        echo
    else
        echo "  using cached suite IR: $irdir  (pass --force to rebuild)"
    fi
    python3 "$REPO/scripts/run_suite.py" --mbcheck "$MBCHECK" --irdir "$irdir" \
        --outdir "$RESULTS/sync-litmus" --json "$RESULTS/table8.json"
    report table8 "$RESULTS/table8.json"
}

case "$CLAIM" in
    smoke)       claim_smoke ;;
    table4)      claim_table4 ;;
    table6)      claim_table6 ;;
    table7)      claim_table7 ;;
    table9)      claim_table9 ;;
    table5)      claim_table5 ;;
    table8)      claim_table8 ;;
    all)
        rc=0
        for c in "${CLAIMS[@]}"; do
            echo; echo "### $c"
            "$0" "$c" --jobs "$JOBS" || rc=1
        done
        exit $rc ;;
    *) die "unknown claim: $CLAIM (try --list)" ;;
esac
