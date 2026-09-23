#!/usr/bin/env bash
# Compile a directory of single-file kernel modules to LLVM IR.
#
#   scripts/build-testsuite-ir.sh <kernel-source> <test-dir> <output-dir>
#
# Flags are taken from a compile command the kernel build already recorded, so
# the modules are compiled exactly as the kernel compiles its own code.  This
# avoids running kbuild per module.
#
# LLVM_SUFFIX (default -18), or CLANG / OPT individually.

set -euo pipefail

[[ $# -eq 3 ]] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

KDIR="$(readlink -f "$1")"
SRCDIR="$(readlink -f "$2")"
OUT="$(readlink -f "$3")"

SUFFIX="${LLVM_SUFFIX:--18}"
CLANG="${CLANG:-clang$SUFFIX}"
OPT="${OPT:-opt$SUFFIX}"

for tool in "$CLANG" "$OPT"; do
    command -v "$tool" >/dev/null \
        || { echo "error: $tool not found on \$PATH" >&2; exit 1; }
done
[[ -d "$KDIR"   ]] || { echo "error: kernel source not found: $KDIR" >&2; exit 1; }
[[ -d "$SRCDIR" ]] || { echo "error: test directory not found: $SRCDIR" >&2; exit 1; }

# Any recorded C compile command will do; they share the kernel-wide flags.
# -print -quit, not a pipe into head, which would trip pipefail on SIGPIPE.
if [[ -z "${TEMPLATE:-}" ]]; then
    TEMPLATE="$(find "$KDIR/kernel" -maxdepth 1 -name '.*.o.cmd' -print -quit 2>/dev/null || true)"
fi
if [[ -z "$TEMPLATE" ]]; then
    TEMPLATE="$(find "$KDIR" -name '.*.o.cmd' -print -quit 2>/dev/null || true)"
fi
[[ -n "$TEMPLATE" ]] || {
    cat >&2 <<EOF
error: no .*.o.cmd files under $KDIR

  The kernel has to be built first so its compile flags are on disk:
    cd $KDIR
    make LLVM=-18 ARCH=arm64 defconfig
    make LLVM=-18 ARCH=arm64 -j\$(nproc)
EOF
    exit 1
}

BASE="$(grep -m1 '^savedcmd_' "$TEMPLATE" | sed 's/^savedcmd_[^=]*:\{0,1\}= //')"

mkdir -p "$OUT"
ok=0; fail=0
for src in "$SRCDIR"/*.c; do
    name="$(basename "$src" .c)"
    [[ "$name" == *.mod ]] && continue
    out="$OUT/$name.ll"

    cmd="$(printf '%s' "$BASE" | sed -E "
        s#^[[:space:]]*\S*clang\S*#$CLANG#;
        s#-Wp,-M(MD|T),\S+ ##g;
        s# -flto=thin##g; s# -fsplit-lto-unit##g;
        s# -g\b##g; s# -gdwarf\S*##g; s# -fno-var-tracking\S*##g;
        s# -c -o \S+ .*\$##;
        s#-DKBUILD_MODFILE='\"[^\"]*\"'#-DKBUILD_MODFILE='\"$name\"'#;
        s#-DKBUILD_BASENAME='\"[^\"]*\"'#-DKBUILD_BASENAME='\"$name\"'#;
        s#-DKBUILD_MODNAME='\"[^\"]*\"'#-DKBUILD_MODNAME='\"$name\"'#")"

    if ( cd "$KDIR" && eval "$cmd -DMODULE -fno-lto -g0 -S -emit-llvm -o '$out.raw' '$src'" ) 2>"$OUT/$name.err"
    then
        # mem2reg: without it every shared access is an alloca load.
        if "$OPT" -passes=mem2reg -S "$out.raw" -o "$out" 2>>"$OUT/$name.err"; then
            rm -f "$out.raw" "$OUT/$name.err"
            ok=$((ok + 1))
            printf '  OK   %s\n' "$name"
            continue
        fi
    fi
    fail=$((fail + 1))
    printf '  FAIL %s  %s\n' "$name" "$(head -c 120 "$OUT/$name.err" | tr '\n' ' ')"
done

echo
echo "  compiled $ok, failed $fail  ->  $OUT"
[[ $fail -eq 0 ]]
