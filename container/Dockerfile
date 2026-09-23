# MBCheck artifact image.
#
#   docker build -f container/Dockerfile -t mbcheck:atc26 container/
#
# Builds SVF, herd7, mbcheck and the Linux v6.19 IR corpus.  See
# container/README.md.

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG MBCHECK_REPO=https://github.com/ssrg-vt/mbcheck.git
ARG MBCHECK_REF=main
ARG LINUX_TAG=v6.19
ARG JOBS=

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git python3 pkg-config \
        curl wget unzip ca-certificates \
        clang-18 llvm-18 llvm-18-dev lld-18 \
        zlib1g-dev libzstd-dev libncurses-dev libgmp-dev \
        bc bison flex libelf-dev libssl-dev rsync cpio kmod zstd \
        opam m4 bubblewrap ocaml \
    && rm -rf /var/lib/apt/lists/*

# ubuntu:24.04 ships a user at uid 1000.  ae takes it so that the uid matches
# in both stages and COPY --from preserves ownership.
RUN touch /var/mail/ubuntu && chown ubuntu /var/mail/ubuntu && userdel -r ubuntu \
    && useradd -m -u 1000 -s /bin/bash ae \
    && install -d -o ae -g ae /opt/mbcheck-build

USER ae
WORKDIR /opt/mbcheck-build
ENV OPAMROOT=/opt/mbcheck-build/opam

# SVF 3.2
RUN git clone --depth 1 --branch SVF-3.2 https://github.com/SVF-tools/SVF.git SVF \
    && cd SVF \
    && LLVM_DIR=/usr/lib/llvm-18 ./build.sh \
    && rm -rf .git

# herd7 7.58.  ocaml-system reuses the distribution compiler rather than
# building a switch from source.
RUN opam init --disable-sandboxing -y --bare \
    && opam switch create herd --packages=ocaml-system \
    && opam install -y --switch=herd herdtools7.7.58 \
    && test -f "$OPAMROOT/herd/share/herdtools7/herd/stdlib.cat" \
    && rm -rf "$OPAMROOT/download-cache" "$OPAMROOT/repo"

ENV HERD7=/opt/mbcheck-build/opam/herd/bin/herd7 \
    HERDLIB=/opt/mbcheck-build/opam/herd/share/herdtools7/herd

# mbcheck
RUN git clone --branch "${MBCHECK_REF}" "${MBCHECK_REPO}" mbcheck \
    && cmake -S mbcheck/src -B mbcheck/build -G Ninja \
             -DCMAKE_BUILD_TYPE=Release -DSVF_DIR=/opt/mbcheck-build/SVF \
    && ninja -C mbcheck/build \
    && mbcheck/scripts/selftest.sh

# Linux v6.19.  LLVM=-18 so the recorded compile commands name clang-18.
RUN git clone --depth 1 --branch "${LINUX_TAG}" \
        https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux \
    && cd linux \
    && make LLVM=-18 ARCH=arm64 defconfig \
    && make LLVM=-18 ARCH=arm64 -j"${JOBS:-$(nproc)}" \
    && rm -rf .git

# IR corpus
RUN mbcheck/scripts/build-kernel-ir.sh /opt/mbcheck-build/linux \
        /opt/mbcheck-build/ir-v6.19 "${JOBS:-$(nproc)}" \
    && cat /opt/mbcheck-build/ir-v6.19/MANIFEST

# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# clang-18 and llvm-18 stay: table8 compiles the microbenchmarks with the
# kernel's flags, and the corpus generator calls llvm-link-18 and opt-18.
RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-18 llvm-18 lld-18 python3 git make \
        bc libelf1 zlib1g libzstd1 libgmp10 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && touch /var/mail/ubuntu && chown ubuntu /var/mail/ubuntu && userdel -r ubuntu \
    && useradd -m -u 1000 -s /bin/bash ae

COPY --from=build --chown=ae:ae /opt/mbcheck-build/opam/herd      /opt/herd
COPY --from=build --chown=ae:ae /opt/mbcheck-build/SVF/Release-build/lib/extapi.bc \
                                                                  /opt/svf/extapi.bc
COPY --from=build --chown=ae:ae /opt/mbcheck-build/mbcheck        /home/ae/mbcheck
COPY --from=build --chown=ae:ae /opt/mbcheck-build/linux          /home/ae/linux-6.19
COPY --from=build --chown=ae:ae /opt/mbcheck-build/ir-v6.19       /home/ae/ir-v6.19

ENV HERD7=/opt/herd/bin/herd7 \
    HERDLIB=/opt/herd/share/herdtools7/herd \
    MBCHECK_EXTAPI_BC=/opt/svf/extapi.bc \
    MBCHECK_CORPUS_V619=/home/ae/ir-v6.19 \
    KDIR=/home/ae/linux-6.19 \
    PATH=/opt/herd/bin:$PATH

USER ae
WORKDIR /home/ae/mbcheck

CMD ["/bin/bash"]
