#!/usr/bin/env bash
# Build the merged LLVM IR corpus from a built kernel tree.
#
#   scripts/build-kernel-ir.sh <kernel-source> <output-dir> [jobs]
#
# One .ll per module, named after KBUILD_MODFILE, mirroring the kernel layout.
# Each module's TUs are recompiled from the build's own recorded commands at
# -g0 -fno-lto, linked with llvm-link and stripped of debug metadata.
#
# The kernel must already be built; the commands come from its .*.o.cmd files:
#   make LLVM=-18 ARCH=arm64 defconfig
#   make LLVM=-18 ARCH=arm64 -j"$(nproc)"
#
# LLVM_SUFFIX (default -18), or CLANG / LLVM_LINK / OPT individually.

set -euo pipefail

usage() {
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}

[[ $# -ge 2 ]] || usage 1
[[ "$1" == "-h" || "$1" == "--help" ]] && usage 0

KDIR="$(readlink -f "$1")"
OUT="$(readlink -f "$2")"
JOBS="${3:-$(nproc)}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[[ -d "$KDIR" ]] || { echo "error: kernel source not found: $KDIR" >&2; exit 1; }

if [[ -z "$(find "$KDIR" -name '.*.o.cmd' -print -quit 2>/dev/null || true)" ]]; then
    cat >&2 <<EOF
error: no .*.o.cmd files under $KDIR

  The kernel has to be built first; the IR is regenerated from the compile
  commands the build records.  For the configuration used in the paper:

    cd $KDIR
    make LLVM=-18 ARCH=arm64 defconfig
    make LLVM=-18 ARCH=arm64 -j$JOBS
EOF
    exit 1
fi

echo "kernel   : $KDIR"
echo "output   : $OUT"
echo "jobs     : $JOBS"
echo

mkdir -p "$OUT"
python3 "$REPO/src/script/gen_module_src_ir.py" -k "$KDIR" -o "$OUT" -j "$JOBS"

MODULES=$(find "$OUT" -name '*.ll' | wc -l)
LINES=$(find "$OUT" -name '*.ll' -print0 | xargs -0 cat | wc -l)

# Skip each file's first line: the ModuleID comment records a temp path and
# is the only part that varies between runs.
SHA=$( cd "$OUT" && find . -name '*.ll' | LC_ALL=C sort \
       | while IFS= read -r f; do printf '%s\n' "$f"; tail -n +2 "$f"; done \
       | sha256sum | cut -d' ' -f1 )
{
    echo "# mbcheck IR corpus manifest"
    echo "kernel_source=$KDIR"
    echo "generated=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "modules=$MODULES"
    echo "ir_lines=$LINES"
    echo "clang=${CLANG:-clang${LLVM_SUFFIX:--18}}"
    echo "llvm_link=${LLVM_LINK:-llvm-link${LLVM_SUFFIX:--18}}"
    echo "opt=${OPT:-opt${LLVM_SUFFIX:--18}}"
    echo "sha256=$SHA"
} > "$OUT/MANIFEST"

echo
echo "corpus   : $MODULES modules, $LINES IR lines"
echo "sha256   : $SHA"
echo "manifest : $OUT/MANIFEST"
