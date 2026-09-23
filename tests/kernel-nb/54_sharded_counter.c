// SPDX-License-Identifier: GPL-2.0
// 54_sharded_counter.c — Sharded (striped) lock-free counter with consistent read
//
// Non-blocking pattern: SHARDED / STRIPED COUNTER (N-writer, 1-reader reduce).
//
// A sharded counter partitions a counter into N shards so that concurrent
// increments from N "writer" threads each hit a different shard, eliminating
// cache-line bouncing.  A reader folds all shards into a global sum.
//
// This is the lock-free variant: each shard is an atomic_long_t; writers use
// atomic_long_add (no CAS loop needed — fetch-and-add is wait-free); the
// reader uses acquire loads of each shard to obtain a consistent lower bound.
//
// Pattern:
//   inc(tid):  atomic_long_add(1, &g_shards[tid % N_SHARDS])  — wait-free
//   read_sum(): sum = 0; for each i: sum += atomic_long_read_acquire(&g_shards[i])
//
// Used in:
//   - Linux kernel's percpu_counter (shard = per-CPU)
//   - Java's LongAdder / Striped64
//   - folly::ThreadCachedInt (litmus 42 is the per-CPU pointer variant)
//
// This litmus is a *peer* pattern: all participants are writers; the reader
// observes a consistent (monotonic) sum.  No producer–consumer asymmetry.
//
// Ordering edges of interest:
//   inc[0]: atomic_long_add(delta_0, &g_shards[0])
//           smp_store_release(&g_done[0], 1)  ← signals completion
//   inc[1]: atomic_long_add(delta_1, &g_shards[1])
//           smp_store_release(&g_done[1], 1)
//   sum:    for each i: smp_load_acquire(&g_done[i])
//           atomic_long_read_acquire(&g_shards[i]) → contributes to total

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-54: sharded counter — N-way wait-free increment + reduce");

#define N_SHARDS  4

/* Per-shard counter (in a real system these would be per-CPU). */
static atomic_long_t g_shards[N_SHARDS];

/* Per-shard "done" flag: writer signals it has finished its batch. */
static atomic_t g_done[N_SHARDS];

/* Result pointer: filled in by the reader with a heap snapshot. */
struct counter_snapshot {
	long shard_vals[N_SHARDS];
	long total;
};
static struct counter_snapshot *g_snapshot;

/* Writer i: adds delta to its shard, then signals done. */
static void sc54_inc(int shard, long delta)
{
	atomic_long_add(delta, &g_shards[shard]);    /* wait-free increment */
	/* Release: increment visible before done flag. */
	atomic_set_release(&g_done[shard], 1);       /* PUBLISH completion */
}

/* Reader: waits for all writers, then takes a consistent snapshot. */
static struct counter_snapshot *sc54_read(void)
{
	struct counter_snapshot *snap;
	long total = 0;
	int i;

	snap = kmalloc(sizeof(*snap), GFP_KERNEL);   /* HeapObjVar */
	if (!snap)
		return NULL;

	for (i = 0; i < N_SHARDS; i++) {
		/* Acquire: see increment before done flag. */
		while (!atomic_read_acquire(&g_done[i]))   /* ACQUIRE */
			cpu_relax();
		snap->shard_vals[i] = atomic_long_read(&g_shards[i]);
		total += snap->shard_vals[i];
	}
	snap->total = total;

	smp_store_release(&g_snapshot, snap);        /* g_snapshot →{heap} */
	return snap;
}

static int __init sc54_init(void)
{
	int i;

	for (i = 0; i < N_SHARDS; i++) {
		atomic_long_set(&g_shards[i], 0);
		atomic_set(&g_done[i], 0);
	}

	/* Four peer writers (simulated sequentially). */
	sc54_inc(0,  10);
	sc54_inc(1,  20);
	sc54_inc(2,  30);
	sc54_inc(3,  40);

	pr_info("sc54: all writers done\n");
	return 0;
}

static void __exit sc54_exit(void)
{
	struct counter_snapshot *snap;
	int i;

	snap = sc54_read();   /* snap →{heap} */
	if (snap) {
		pr_info("sc54: total=%ld\n", snap->total);
		for (i = 0; i < N_SHARDS; i++)
			pr_info("sc54:   shard[%d]=%ld\n", i, snap->shard_vals[i]);
		kfree(snap);
	}
}

module_init(sc54_init);
module_exit(sc54_exit);
