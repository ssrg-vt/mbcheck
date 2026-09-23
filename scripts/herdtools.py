"""Locate herd7 and its .cat files.

A source-built herd7 cannot find stdlib.cat unless the directory is passed
explicitly, and fails per litmus test rather than up front.
"""

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

PROBE = """AArch64 mbcheck-probe
{
0:X0=x; 1:X0=x;
}
 P0           | P1            ;
 MOV W1,#1    | LDR W1,[X0]   ;
 STR W1,[X0]  |               ;
exists (1:X1=1)
"""


def resolve_herd7(explicit=None):
    """Explicit path, else $HERD7, else the first herd7 on $PATH."""
    if explicit:
        return Path(explicit)
    env = os.environ.get("HERD7")
    if env:
        return Path(env)
    for name in ("herd7", "herd.exe"):
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def resolve_herdlib(herd7_bin, explicit=None):
    """Explicit, else $HERDLIB, else derived from the binary location."""
    if explicit:
        return str(explicit)
    env = os.environ.get("HERDLIB")
    if env:
        return env
    if herd7_bin is None:
        return None
    for parent in Path(herd7_bin).resolve().parents:
        for rel in ("herd/libdir", "share/herdtools7/herd"):
            cand = parent / rel
            if (cand / "stdlib.cat").is_file():
                return str(cand)
    return None


def argv(herd7_bin, herdlib, litmus):
    cmd = [str(herd7_bin)]
    if herdlib:
        cmd += ["-set-libdir", str(herdlib)]
    cmd.append(str(litmus))
    return cmd


def probe(herd7_bin, herdlib):
    """Run herd7 on a trivial test.  None on success, else a message."""
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "probe.litmus"
        path.write_text(PROBE)
        try:
            proc = subprocess.run(argv(herd7_bin, herdlib, path),
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  text=True, timeout=60)
        except Exception as exc:
            return f"could not execute {herd7_bin}: {exc}"
    if any(line.strip() in ("Ok", "No") for line in proc.stdout.splitlines()):
        return None
    first = proc.stdout.strip().splitlines()
    detail = first[0] if first else f"exit {proc.returncode}, no output"
    hint = ""
    if "stdlib.cat" in proc.stdout:
        hint = ("\n  herd7 cannot find its .cat files.  Set $HERDLIB to the "
                "directory holding stdlib.cat; a source build keeps it at "
                "<herdtools7>/herd/libdir.")
    return f"herd7 did not answer on a trivial litmus test: {detail}{hint}"
