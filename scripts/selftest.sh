#!/usr/bin/env bash
# Check a freshly built mbcheck.  Needs no kernel tree and no IR corpus.
#
#   scripts/selftest.sh [path/to/mbcheck]
#
# Compiles one message-passing pattern and runs the pipeline over it, then
# checks the emitted litmus test with herd7 if it is reachable.
#
# LLVM_SUFFIX (default -18) or CLANG; HERD7 and HERDLIB for the oracle.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBCHECK="${1:-${MBCHECK:-$REPO/build/mbcheck}}"
CLANG="${CLANG:-clang${LLVM_SUFFIX:--18}}"

[[ -x "$MBCHECK" ]] || {
    echo "error: mbcheck not found: $MBCHECK" >&2
    echo "  Build it first; see README.md." >&2
    exit 1
}
command -v "$CLANG" >/dev/null || {
    echo "error: $CLANG not found on \$PATH" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/probe.c" <<'PROBE'
/* Correctly ordered message passing: forbidden state must be unreachable. */
int g_data, g_flag;

void publish(void)
{
	g_data = 1;
	__asm__ __volatile__("dmb ishst" ::: "memory");
	g_flag = 1;
}

int consume(void)
{
	int f = g_flag;
	__asm__ __volatile__("dmb ishld" ::: "memory");
	return f ? g_data : 0;
}
PROBE

echo "mbcheck : $MBCHECK"
echo "compiler: $CLANG"
echo

"$CLANG" --target=aarch64-linux-gnu -O2 -S -emit-llvm \
         -o "$WORK/probe.ll" "$WORK/probe.c"

hint_extapi() {
    if grep -qs "extapi.bc" "$WORK/summary.txt" "$WORK/err.txt"; then
        echo "  SVF cannot find extapi.bc.  Reconfigure and rebuild once SVF" >&2
        echo "  is installed, or set \$MBCHECK_EXTAPI_BC.  See README.md." >&2
    fi
}

"$MBCHECK" --summary "$WORK/probe.ll" > "$WORK/summary.txt" 2>"$WORK/err.txt" || {
    echo "FAIL: mbcheck exited non-zero" >&2
    head -5 "$WORK/err.txt" "$WORK/summary.txt" >&2
    hint_extapi
    exit 1
}

stat_of() { sed -n "s/^STATS:$1=//p" "$WORK/summary.txt" | head -1; }

anchors=$(stat_of "anchors:total")
causal=$(stat_of "causal:total")
litmus=$(stat_of "litmus:total")

printf '  anchors detected   : %s\n' "${anchors:-0}"
printf '  causal brackets    : %s\n' "${causal:-0}"
printf '  litmus synthesised : %s\n' "${litmus:-0}"

rc=0
if [[ "${anchors:-0}" -lt 2 || "${causal:-0}" -lt 1 || "${litmus:-0}" -lt 1 ]]; then
    echo
    echo "FAIL: expected at least 2 anchors, 1 bracket and 1 litmus test." >&2
    hint_extapi
    exit 1
fi

HERD7_BIN="${HERD7:-$(command -v herd7 || command -v herd.exe || true)}"
if [[ -n "$HERD7_BIN" && -x "$HERD7_BIN" ]]; then
    "$MBCHECK" --litmus "$WORK/litmus" "$WORK/probe.ll" >/dev/null 2>&1 || true
    lib="${HERDLIB:-}"
    if [[ -z "$lib" ]]; then
        d="$(dirname "$(readlink -f "$HERD7_BIN")")"
        while [[ "$d" != "/" ]]; do
            for rel in herd/libdir share/herdtools7/herd/libdir; do
                [[ -f "$d/$rel/stdlib.cat" ]] && { lib="$d/$rel"; break 2; }
            done
            d="$(dirname "$d")"
        done
    fi
    for lf in "$WORK"/litmus/*.litmus; do
        [[ -e "$lf" ]] || continue
        v="$("$HERD7_BIN" ${lib:+-set-libdir "$lib"} "$lf" 2>/dev/null \
             | grep -Ex '(Ok|No)' | head -1 || true)"
        printf '  herd7 verdict      : %s\n' "${v:-no answer}"
        if [[ "$v" != "Ok" ]]; then
            echo >&2
            echo "FAIL: herd7 did not answer Ok on a correctly ordered test." >&2
            [[ -z "$v" ]] && echo "  It probably cannot find its .cat files; set \$HERDLIB." >&2
            rc=1
        fi
    done
else
    echo '  herd7 verdict      : skipped (herd7 not found)'
fi

echo
[[ $rc -eq 0 ]] && echo "PASS: mbcheck builds and analyses correctly."
exit $rc
