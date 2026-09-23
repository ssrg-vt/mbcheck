#!/usr/bin/env python3
"""Compile Linux kernel modules from source to LLVM IR.

Groups translation units by KBUILD_MODFILE (taken from the .*.o.cmd files the
kernel build records), recompiles each at -g0 -fno-lto, links the per-TU IR
and strips debug metadata, writing <outroot>/<KBUILD_MODFILE>.ll.

  gen_module_src_ir.py -k <kernel-source> -o <output-root> [-j N] [-v] [-n]
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

_SUFFIX   = os.environ.get("LLVM_SUFFIX", "-18")
LLVM_LINK = os.environ.get("LLVM_LINK", f"llvm-link{_SUFFIX}")
OPT       = os.environ.get("OPT", f"opt{_SUFFIX}")
# Pin the compiler: a newer clang emits IR this LLVM cannot parse.
CLANG     = os.environ.get("CLANG", f"clang{_SUFFIX}")


def parse_o_cmd(cmd_file):
    """
    Returns (src, modfile, cmd) for a clang compile command, else Nones.
    """
    try:
        text = cmd_file.read_text(errors="replace")
    except OSError:
        return None, None, None

    cmd_m = re.search(r"^savedcmd_\S+ := (.+)", text, re.MULTILINE)
    if not cmd_m:
        return None, None, None
    raw_cmd = cmd_m.group(1)

    if not raw_cmd.lstrip().startswith("clang"):
        return None, None, None

    src_m = re.search(r"^source_\S+ := (\S+)", text, re.MULTILINE)
    src = src_m.group(1) if src_m else None

    mf_m = re.search(r'KBUILD_MODFILE=[\'"]+"?([^\'"\s]+)', raw_cmd)
    modfile = mf_m.group(1) if mf_m else None

    return src, modfile, raw_cmd


def compile_to_ir(linux, raw_cmd, src, out_ll):
    """
    Rewrite a clang -c command as -S -emit-llvm -g0.  True on success.
    """
    if not src or src.lower().endswith((".s", ".asm")):
        return False
    if not (linux / src).exists():
        return False

    cmd = raw_cmd

    cmd = re.sub(r"^\s*\S*clang\S*", CLANG, cmd, count=1)

    # Remove dependency tracking
    cmd = re.sub(r"-Wp,-M(?:MD|T),\S+\s*", "", cmd)

    # Remove ThinLTO, add -fno-lto
    cmd = re.sub(r"\s+-fno-lto\b",        "", cmd)
    cmd = re.sub(r"\s+-flto(?:=\S+)?\b",  "", cmd)
    cmd = re.sub(r"\s+-fsplit-lto-unit\b", "", cmd)
    cmd = cmd.rstrip() + " -fno-lto"

    # Remove debug flags (replaced by -g0)
    cmd = re.sub(r"\s+-g\b",                 "", cmd)
    cmd = re.sub(r"\s+-g[0-9]\b",            "", cmd)
    cmd = re.sub(r"\s+-gdwarf\S*",           "", cmd)
    cmd = re.sub(r"\s+-fno-var-tracking\S*", "", cmd)

    # Replace -c -o <obj> with -S -emit-llvm -g0 -o <out_ll>
    cmd = re.sub(r"\s+-c -o \S+", "", cmd)
    quoted = shlex.quote(str(out_ll))
    parts = cmd.rsplit(None, 1)
    if len(parts) == 2:
        before, source_arg = parts
        cmd = f"{before} -S -emit-llvm -g0 -o {quoted} {source_arg}"
    else:
        cmd = f"{cmd} -S -emit-llvm -g0 -o {quoted}"

    try:
        r = subprocess.run(
            cmd,
            shell=True,
            cwd=str(linux),
            capture_output=True,
            timeout=120,
        )
        return r.returncode == 0 and out_ll.is_file() and out_ll.stat().st_size > 0
    except Exception:
        return False


def process_module(linux_str, outroot_str, modfile, tu_list):
    """
    Compile a module's TUs, link, strip debug, write .ll.
    """
    linux   = Path(linux_str)
    outroot = Path(outroot_str)

    outfile = outroot / f"{modfile}.ll"
    outfile.parent.mkdir(parents=True, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="mbcheck_src_ir_")
    try:
        ir_files = []
        for i, (src, raw_cmd) in enumerate(tu_list):
            out_ll = Path(tmp) / f"{i:04d}_{Path(src).stem}.ll"
            if compile_to_ir(linux, raw_cmd, src, out_ll):
                ir_files.append(out_ll)

        if not ir_files:
            if all(src.lower().endswith((".s", ".asm")) for src, _ in tu_list):
                return modfile, None, "assembly only"
            return modfile, False, "no sources compiled"

        merged_bc = Path(tmp) / "merged.bc"
        r = subprocess.run(
            [LLVM_LINK] + [str(f) for f in ir_files] + ["-o", str(merged_bc)],
            capture_output=True,
            timeout=300,
        )
        if r.returncode != 0:
            msg = r.stderr.decode(errors="replace")[:200].strip()
            return modfile, False, f"llvm-link: {msg}"

        r = subprocess.run(
            [OPT, "--strip-debug", "-S", str(merged_bc), "-o", str(outfile)],
            capture_output=True,
            timeout=300,
        )
        if r.returncode != 0 or not outfile.is_file() or outfile.stat().st_size == 0:
            msg = r.stderr.decode(errors="replace")[:200].strip()
            return modfile, False, f"opt: {msg}"

        return modfile, True, ""

    except Exception as exc:
        return modfile, False, str(exc)

    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def collect_modules(linux):
    """
    {modfile: [(src, cmd), ...]} over all .*.o.cmd files.
    """
    modules = defaultdict(list)
    for cmd_file in sorted(linux.rglob(".*.o.cmd")):
        src, modfile, raw_cmd = parse_o_cmd(cmd_file)
        if src and modfile and raw_cmd:
            modules[modfile].append((src, raw_cmd))
    return dict(modules)


def main():
    parser = argparse.ArgumentParser(
        description="Compile Linux kernel modules from source to LLVM IR"
    )
    parser.add_argument("-k", "--linux", required=True,
                        help="Linux source root, already built")
    parser.add_argument("-o", "--outroot", required=True,
                        help="Output root for the nested .ll tree")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("-n", "--dry-run", action="store_true")
    args = parser.parse_args()

    linux = Path(args.linux).resolve()
    if not linux.is_dir():
        print(f"ERROR: Linux source not found: {linux}", file=sys.stderr)
        sys.exit(1)

    for tool in (CLANG, LLVM_LINK, OPT):
        if shutil.which(tool) is None:
            print(f"ERROR: {tool} not found on $PATH.  Set $LLVM_SUFFIX (e.g. "
                  f"-18), or $CLANG, $LLVM_LINK and $OPT individually.",
                  file=sys.stderr)
            sys.exit(1)

    outroot = Path(args.outroot).resolve()
    outroot.mkdir(parents=True, exist_ok=True)

    print(f"Linux source : {linux}")
    print(f"Output root  : {outroot}")
    print(f"Compiler     : {CLANG}")
    print(f"Linker       : {LLVM_LINK}")
    print(f"Parallel jobs: {args.jobs}")
    print("Scanning .o.cmd files...", end=" ", flush=True)

    modules  = collect_modules(linux)
    total    = len(modules)
    tu_total = sum(len(v) for v in modules.values())
    print(f"found {total} modules ({tu_total} translation units).")

    if args.dry_run:
        print("\nTop 20 modules by TU count:")
        for mf, tus in sorted(modules.items(), key=lambda x: -len(x[1]))[:20]:
            print(f"  {len(tus):3d} TUs  {mf}")
        return

    ok = fail = skipped = 0

    with ProcessPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                process_module,
                str(linux), str(outroot), mf, tus,
            ): mf
            for mf, tus in modules.items()
        }

        done = 0
        for future in as_completed(futures):
            done += 1
            mf_key = futures[future]
            try:
                mf, success, msg = future.result()
                if success:
                    ok += 1
                    if args.verbose:
                        print(f"[{done}/{total}] OK   {mf}")
                elif success is None:
                    skipped += 1
                    if args.verbose:
                        print(f"[{done}/{total}] SKIP {mf}: {msg}")
                else:
                    fail += 1
                    if args.verbose:
                        print(f"[{done}/{total}] FAIL {mf}: {msg}")
            except Exception as exc:
                fail += 1
                print(f"[{done}/{total}] ERROR {mf_key}: {exc}")

    print(f"\n=== Done ===")
    print(f"  IR written : {ok}")
    print(f"  Skipped    : {skipped}  (assembly-only modules: no C to compile)")
    print(f"  Failed     : {fail}")
    print(f"  Total      : {total}")

    if ok == 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
