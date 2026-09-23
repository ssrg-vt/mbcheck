# ── Non-blocking pattern litmus tests ──────────────────────────────────────
#
# Each file demonstrates one non-blocking (lock-free/wait-free) concurrency
# pattern drawn from the Linux kernel and the folly synchronization library.
# All patterns are expressed using native kernel APIs whose ordering semantics
# are documented in ../../docs/kernel-memory-ordering-apis.md.
#
# Pattern taxonomy:
#   01-04  Message Passing (MP) — release-store / acquire-load pointer publish
#   05-07  One-shot Event / Baton — atomic flag or kernel completion
#   08-10  Lock-free Stack & Queue — Treiber stack, SPSC ring, MPSC list
#   11-12  CAS update & Lazy-init — cmpxchg loops
#   13-15  Fetch-and-op & Ownership — fetch_add, fetch_or, xchg
#   16-19  Refcount & Once-init — refcount_t, DO_ONCE style
#   20-22  Write-once-read-many & Seqlock
#   23-26  Asymmetric barrier, Epoch reclaim, Generation counter, Hazard ptr
#   27-29  MPMC ring, EventCount, Flat-combining delegation
#   30-32  LIFO semaphore, Saturating semaphore, Consistent snapshot
#   33-35  RCU list, Per-CPU aggregation, Atomic futex-like notification
#   36-40  Chase-Lev deque, Split-ordered list, CLH lock, MCS lock, Skiplist
#   41-46  Token bucket, Per-CPU int, AtomicStruct, Indexed mempool,
#          Herlihy UC, Concurrent bitset
#   47-59  Non-producer/consumer peer patterns:
#          47: Multi-writer RCU (writer-writer exclusion)
#          48: MPMC bounded ring buffer (Vyukov-style)
#          49: Countdown latch (N-way join)
#          50: Lock-free freelist/object pool (MPMC recycle)
#          51: Consistent snapshot with multiple independent writers
#          52: Harris lock-free linked list (logical deletion, N-peer)
#          53: Scalable RW-lock (non-blocking readers)
#          54: Sharded counter (N-writer wait-free + reduce)
#          55: Obstruction-free double-ended queue (OF deque)
#          56: Two-peer rendezvous barrier (symmetric, no roles)
#          57: Work-stealing scheduler (N-peer push/steal)
#          58: Lock-free exchanger (symmetric value swap)
#          59: Combining tree node (peer fetch-and-add combining)

MODULES := \
	01_mp_release_acquire
# 	01_mp_release_acquire \
# 	02_mp_write_once_mb \
# 	03_mp_rcu \
# 	04_mp_seqcount \
# 	05_event_baton \
# 	06_event_completion \
# 	07_event_store_poll \
# 	08_stack_treiber \
# 	09_spsc_ring \
# 	10_mpsc_list \
# 	11_cas_update \
# 	12_cas_lazy_init \
# 	13_fetch_add_refcnt \
# 	14_fetch_or_flags \
# 	15_xchg_ownership \
# 	16_refcount_get_put \
# 	17_refcount_inc_not_zero \
# 	18_once_init \
# 	19_double_checked \
# 	20_worm_pointer \
# 	21_worm_flag \
# 	22_seqlock_rw \
# 	23_asymmetric_barrier \
# 	24_epoch_reclaim \
# 	25_generation_counter \
# 	26_hazptr_retire \
# 	27_mpmc_ring \
# 	28_eventcount \
# 	29_flat_combine \
# 	30_lifosem \
# 	31_saturating_sem \
# 	32_snapshot_seqcount \
# 	33_rcu_list \
# 	34_percpu_aggr \
# 	35_atomic_notify \
# 	36_chase_lev_deque \
# 	37_split_ordered_list \
# 	38_clh_lock \
# 	39_mcs_lock \
# 	40_skiplist \
# 	41_token_bucket \
# 	42_percpu_int \
# 	43_atomic_struct \
# 	44_indexed_mempool \
# 	45_herlihy_uc \
# 	46_concurrent_bitset \
# 	47_rcu_writer_exclusion \
# 	48_mpmc_ring \
# 	49_countdown_latch \
# 	50_freelist_pool \
# 	51_snapshot_multiwriter \
# 	52_harris_list \
# 	53_rwlock_nb \
# 	54_sharded_counter \
# 	55_ofdq \
# 	56_rendezvous \
# 	57_work_stealing \
# 	58_exchanger \
# 	59_combining_tree

obj-m := $(patsubst %,%.o,$(MODULES))

KDIR          ?= $(error set KDIR to a built Linux kernel source tree)
CLANG_VERSION := 18
ARCH          := arm64

.PHONY: all module ir clean

all: module ir

# ── Build all .ko files ─────────────────────────────────────────────────────
module:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=-$(CLANG_VERSION) ARCH=$(ARCH) V=1 \
	    modules 2>&1 | tee .build.log

# ── Emit mem2reg-normalised LLVM IR for every module ────────────────────────
ir: module
	@for mod in $(MODULES); do \
	    grep "clang-$(CLANG_VERSION).*$${mod}\.c" .build.log | head -1 | \
	        sed "s|.*clang-$(CLANG_VERSION)||" | \
	        sed "s|-c -o $${mod}\.o $${mod}\.c.*||" | \
	        tr -s ' ' > .cflags_$${mod}; \
	    eval clang-$(CLANG_VERSION) $$(cat .cflags_$${mod}) \
	        -Xclang -disable-O0-optnone -g0 -S -emit-llvm -o - $${mod}.c \
	        | opt-$(CLANG_VERSION) -passes=mem2reg -S -o $${mod}.ll; \
	    echo "  IR  $${mod}.ll"; \
	done

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=-$(CLANG_VERSION) ARCH=$(ARCH) clean
	rm -f $(patsubst %,%.ll,$(MODULES)) \
	      $(patsubst %,.cflags_%,$(MODULES)) \
	      .build.log
