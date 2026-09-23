# MBCheck

Static analysis that turns a Linux kernel build into synthesised litmus tests
and checks them against the Linux Kernel Memory Model with herd7.

Artifact for *MBCheck: Practical Static Analysis Framework for Detecting
Memory Ordering Violations under WMM* (USENIX ATC '26).

The claims can be reproduced two ways: with the
[prebuilt container image](#dockerized-setup), which needs nothing installed,
or by [building from source](#manual-setup).

## Contents

- [Dockerized setup](#dockerized-setup)
- [Requirements](#requirements)
- [Manual setup](#manual-setup)
  - [1. Packages](#1-packages)
  - [2. SVF 3.2](#2-svf-32)
  - [3. herd7 7.58](#3-herd7-758)
  - [4. mbcheck](#4-mbcheck)
  - [5. Kernel tree](#5-kernel-tree)
  - [6. IR corpus](#6-ir-corpus)
  - [7. Environment](#7-environment)
- [Quick start](#quick-start)
- [Claims](#claims)
  - [smoke](#smoke)
  - [table4](#table4)
  - [table5](#table5)
  - [table6](#table6)
  - [table7](#table7)
  - [table8](#table8)
  - [table9](#table9)
  - [all](#all)
- [Layout](#layout)
- [Troubleshooting](#troubleshooting)
- [Licence](#licence)

## Requirements

For the manual setup. The container image carries all of it.

| | Version |
|---|---|
| LLVM / Clang | 18 (newer will not work: mbcheck cannot parse its IR) |
| SVF | 3.2, built against LLVM 18 |
| herd7 | 7.58 |
| Python | 3.10+, standard library only |
| CMake | 3.16+ with Ninja |
| Linux kernel | v6.19, built |

60 GB disk, 8 GB RAM, any x86-64. The kernel is cross-compiled for arm64 with
Clang; nothing runs on arm64.

| Step | Time |
|---|---|
| Build SVF | 20-40 min |
| Build mbcheck | < 2 min |
| Build a kernel | 20-40 min |
| Generate a corpus | ~8 min at 16 jobs |
| All claims | ~20 min |

## Manual setup

Ubuntu 24.04. Steps 1 to 6 take about two hours, most of it compiling.

### 1. Packages

```sh
sudo apt update
sudo apt install -y \
    build-essential cmake ninja-build git python3 pkg-config \
    curl wget unzip ca-certificates \
    clang-18 llvm-18 llvm-18-dev lld-18 \
    zlib1g-dev libzstd-dev libncurses-dev \
    bc bison flex libelf-dev libssl-dev rsync cpio kmod zstd \
    opam m4 bubblewrap
```

### 2. SVF 3.2

```sh
cd ~
git clone --branch SVF-3.2 https://github.com/SVF-tools/SVF.git
cd SVF
export LLVM_DIR=/usr/lib/llvm-18
./build.sh
ls Release-build/lib/libSvfCore.a Release-build/lib/extapi.bc
```

`SVF_DIR` below is the directory you cloned into.

### 3. herd7 7.58

```sh
opam init --disable-sandboxing -y
eval "$(opam env)"
opam install -y herdtools7.7.58
herd7 -version
```

Or.

An opam install puts the `.cat` files where herd7 finds them. A source build
does not, so set `HERD7` and `HERDLIB`:

```sh
git clone https://github.com/herd/herdtools7.git
cd herdtools7 && make
export HERD7=$PWD/_build/default/herd/herd.exe
export HERDLIB=$PWD/herd/libdir
```

### 4. mbcheck

```sh
cd ~
git clone <repository-url> mbcheck
cd mbcheck
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSVF_DIR=$HOME/SVF
ninja -C build
scripts/selftest.sh
```

`LLVM_DIR` defaults to `/usr/lib/llvm-18/lib/cmake/llvm`; pass
`-DLLVM_DIR=<prefix>/lib/cmake/llvm` for LLVM installed elsewhere.

`selftest.sh` compiles its own translation unit, so it needs no kernel tree
and no corpus. It reports 2 anchors, 2 causal brackets, 1 litmus test, and an
`Ok` from herd7 if herd7 is installed.

### 5. Kernel tree

The corpus is regenerated from the compile commands a build records in its
`.*.o.cmd` files, so the kernel has to be built once.

```sh
cd ~
git clone --depth 1 --branch v6.19 \
    https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-6.19
cd linux-6.19
make LLVM=-18 ARCH=arm64 defconfig
make LLVM=-18 ARCH=arm64 -j"$(nproc)"
```

### 6. IR corpus

```sh
cd ~/mbcheck
scripts/build-kernel-ir.sh ~/linux-6.19 ~/ir-v6.19
```

One `.ll` per module, named after its `KBUILD_MODFILE`, mirroring the kernel
layout. For v6.19, 3218 modules are found and 58 skipped: 57 are assembly
only, one is the generated export table, leaving 3160 modules and 6,856,865
IR lines.

Each run writes a `MANIFEST` with the module count, IR line count and a
checksum over the corpus. Module and line counts are the portable check; the
checksum matches only for the same Clang point release, since the IR carries
its version string.

The corpus is generated at `-g0` and then run through `opt --strip-debug`, so
the kernel's own debug-info configuration does not affect it.

### 7. Environment

```sh
export MBCHECK_CORPUS_V619=~/ir-v6.19
export KDIR=~/linux-6.19        # table8 only
```

`HERD7` and `HERDLIB` only when herd7 is not on `$PATH` or its `.cat` files
cannot be found from the binary.

## Dockerized setup

A prebuilt image with the toolchain, the Linux v6.19 tree and the IR corpus
already in place. mbcheck is compiled in the image, so nothing has to be
built and the claims can be run straight away.

```sh
docker pull ghcr.io/ssrg-vt/mbcheck:atc26
docker run --rm -it ghcr.io/ssrg-vt/mbcheck:atc26
```

The shell opens in `/home/ae/mbcheck` as user `ae`, with the corpus, kernel
tree and herd7 paths already set:

```sh
scripts/run.sh smoke
scripts/run.sh table4
```

Run the claims in one session: tables 4 to 9 share a cached sweep, and
`--rm` discards it when the container exits. Skip to [Claims](#claims) for
what each one reports. 

## Quick start

```sh
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSVF_DIR=$HOME/SVF
ninja -C build
scripts/selftest.sh

scripts/build-kernel-ir.sh ~/linux-6.19 ~/ir-v6.19
export MBCHECK_CORPUS_V619=~/ir-v6.19

scripts/run.sh smoke
scripts/run.sh table4
```

## Claims

One command each; each prints the values it measured. Tables 4 to 9 derive
from one sweep, cached under `results/`.

### smoke

```sh
scripts/run.sh smoke
```

Not a paper claim: 50 modules of the v6.19 corpus, under a minute. Run it
first.

**Expectation:** 50 modules, 767 anchors, 174 causal brackets, 1,258 raw
pairs, 30 surviving, 3 litmus tests, 3 `Ok` from herd7, 0 errors.

Non-zero herd7 errors mean herd7 cannot find its `.cat` files; set `HERDLIB`.

### table4

```sh
scripts/run.sh table4
```

Table 4, *Linux kernel sweep (v6.19) result*: modules, anchors, causal
brackets, function pairs before and after filtering, litmus tests and
violations. About 7 minutes at 16 jobs.

**Expectation:** 3,160 modules, 6,856,865 IR lines, ~169,500 anchors, ~37,750
causal brackets, ~857,400 raw pairs, ~120,400 surviving, ~25,700 litmus tests,
31 violations.

IR lines are lower than the paper's because the corpus is built at `-g0`; the
paper's figure retains the debug info the kernel's own commands emit.

Counts move with how many modules finish inside `--timeout`, 120 s by default.

### table5

```sh
scripts/run.sh table5
```

Table 5, *Runtime overhead of MBCheck framework for the entire kernel sweep*:
share of time per pipeline stage, plus peak RSS. Cached.

**Expectation:** transpiler ~54% of attributed CPU, herd7 ~18%, function
pairing ~10%, bracket extraction ~8.6%, mutual-exclusion filtering ~7%, the
rest under 1% each; peak RSS around 5 GB.

Absolute times and memory depend on the machine.

### table6

```sh
scripts/run.sh table6
```

Table 6, *Top 4 anchor instructions and external functions with ordering
information*. Cached.

**Expectation:** `dmb oshld` 4,054, `dmb oshst` 3,943, `stlxr` 3,197,
`dmb ish` 2,866, `_raw_spin_lock` 5,690, `__kmalloc_cache_noprof` 1,736,
`__kmalloc_noprof` 902, `flush_work` 61.

### table7

```sh
scripts/run.sh table7
```

Table 7, *Top 4 heuristics hits across the checks performed during the kernel
sweep*. Cached.

**Expectation:** CallGraph 42,574, Lifecycle 26,504, LockAlias 19,755,
SafeAllocation 10,502, over 857,436 candidate pairs.

`ReadRead` and `BenignAnchor` are reported alongside; the paper's table does
not list them.

### table8

```sh
scripts/run.sh table8
```

Table 8, *Capability of a WMM sanitizer to precisely detect non-blocking
patterns*, MBCheck column: the 23 correctly synchronised cases of Table 10,
all of which should be true negatives. About 2 minutes. Needs `$KDIR`.

**Expectation:** 23 true negatives, 0 false positives.

### table9

```sh
scripts/run.sh table9
```

Table 9, *Kernel result sweep by MBCheck*: the per-module distribution of the
violations behind the true-positive, borderline and false-positive counts.
Cached.

**Expectation:** 31 violations across 7 modules: `mm/swapfile` 13,
`drivers/tty/vt/keyboard` 6,
`drivers/firmware/arm_scmi/transports/scmi_transport_optee` 3,
`kernel/events/callchain` 3, `kernel/rcu/tree` 3,
`drivers/soc/fsl/qbman/qman_ccsr` 2, `kernel/audit` 1.

Table 9 is being revised: the paper's figures are from Linux v6.19, and the
revision restates the table against v7.3-rc4. The counts above are what the
current tool reports on v6.19 and will not match the published table.

### all

```sh
scripts/run.sh all
```

Every claim above, about 20 minutes once the corpus exists.

## Layout

```
src/            passes, herd7 transpiler, driver
src/script/     sweep runner, IR corpus generator, patch synthesis
src/spec/       TLA+ specification for the transpiler obligations
scripts/        claim runner, corpus and test-suite builders, self-test
container/      Dockerfile for the prebuilt image
tests/          kernel-sync (23 synchronisation idioms), kernel-nb, kernel-ptr
results/        sweep logs, litmus tests, JSON summaries (generated)
```

`src/include/lib/ext_fn_summary.h` and `src/include/lib/anchors.h` carry the
external-function ordering table and the anchor API classification, compiled
in rather than read at run time.

## Troubleshooting

| | |
|---|---|
| `mbcheck requires LLVM 18; found ...` | `-DLLVM_DIR=` points at another version |
| `SVF include directory not found` | `-DSVF_DIR=` is the build directory; it wants the source root |
| `Failed to locate "extapi.bc"` | mbcheck built before SVF finished; reconfigure and rebuild |
| `herd7 not found` | not on `$PATH`; set `HERD7` |
| `herd7 did not answer on a trivial litmus test` | set `HERDLIB` |
| `IR corpus not found` | `MBCHECK_CORPUS_V619` unset, or corpus not built |
| `no .*.o.cmd files under ...` | the kernel tree is not built |
| `llvm-link: error: expected type` | corpus built by a Clang newer than 18; check `$CLANG` and `$LLVM_SUFFIX` |
| a `.ll` file "does not exist" | none are shipped; see steps 4 and 6 |

## Licence

MIT, except the kernel test modules under `tests/`, which are GPL-2.0. See
[LICENSE](LICENSE).
