// SPDX-License-Identifier: GPL-2.0
// 59_combining_tree.c — Lock-free combining tree (tournament/fetch-and-add tree)
//
// Non-blocking pattern: COMBINING TREE (Gottlieb et al. 1983 / Shavit & Zemach 1996).
//
// A combining tree provides a scalable fetch-and-add by routing concurrent
// increments through a binary tree.  At each internal node, two arriving
// threads "combine" their operations: one acts as a "passive" combiner (waits)
// and the other as the "active" propagator (carries the combined value up the
// tree).  The root applies the combined value to the real counter.
//
// Unlike flat combining (29) where a single combiner runs all operations, the
// combining tree has multiple concurrent combiners at different tree levels.
// This is a true peer pattern: any two simultaneous threads become each other's
// combiner partner at their first shared ancestor node.
//
// This litmus implements a single-level combining node (the simplest non-trivial
// case) to keep the pointer analysis tractable:
//
//   Phase 1 (precombining): CAS node->state IDLE→WAITING to become passive,
//                           or WAITING→RESULT to act as active combiner.
//   Phase 2 (combining):    Active combiner reads passive's delta, propagates
//                           combined (my_delta + passive_delta) to counter.
//   Phase 3 (distribute):   Active stores result; passive reads it via acquire.
//
// Ordering edges of interest:
//   passive:  kmalloc() → slot →{heap}; store_release(slot->delta, my_delta)
//             CAS(node->state, IDLE, WAITING) → node in WAITING state
//             spin load_acquire(slot->result_ready) == 1
//             READ_ONCE(slot->result) →{heap or scalar}
//   active:   load_acquire(node->passive_slot) → slot →{heap}
//             READ_ONCE(slot->delta)
//             combined = my_delta + slot->delta; apply to counter
//             store_release(slot->result, counter_val)
//             store_release(slot->result_ready, 1) → wakes passive

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-59: combining tree node — peer combining fetch-and-add");

enum cnode_state { CN_IDLE = 0, CN_WAITING = 1, CN_BUSY = 2 };

struct passive_slot {
	int   delta;          /* passive thread's increment */
	int   result;         /* result written by active */
	atomic_t result_ready;/* 0 = pending, 1 = done */
};

/* The combining node: one slot for the passive thread's data. */
struct cnode {
	atomic_t          state;         /* IDLE / WAITING / BUSY */
	struct passive_slot *passive_slot;/* pointer to passive's heap slot */
};

/* The real shared counter. */
static atomic_long_t g_counter = ATOMIC_LONG_INIT(0);

/* One combining node (root). */
static struct cnode g_node;

/* Perform a combined fetch-and-add of 'delta'.  Returns the pre-add value. */
static long ctree_fetch_add(int delta)
{
	struct passive_slot *slot;
	enum cnode_state old_state;
	long result;

	/* Try to become the passive (WAITING) thread. */
	slot = kmalloc(sizeof(*slot), GFP_KERNEL);   /* HeapObjVar */
	if (!slot)
		return atomic_long_fetch_add(delta, &g_counter);  /* fallback */

	slot->delta = delta;
	slot->result = 0;
	atomic_set(&slot->result_ready, 0);

	smp_store_release(&g_node.passive_slot, slot);   /* passive_slot →{heap} */

	old_state = atomic_cmpxchg(&g_node.state, CN_IDLE, CN_WAITING);

	if (old_state == CN_IDLE) {
		/* We are the passive thread: wait for active to fill result. */
		while (!atomic_read_acquire(&slot->result_ready))  /* ACQUIRE */
			cpu_relax();
		result = READ_ONCE(slot->result);                  /* result scalar */
		atomic_set(&g_node.state, CN_IDLE);
	} else {
		/* We are the active thread: combine with passive's delta. */
		struct passive_slot *pslot;
		int combined;
		long pre;

		/* Wait until passive has published its slot. */
		while (atomic_read(&g_node.state) != CN_WAITING)
			cpu_relax();

		pslot    = smp_load_acquire(&g_node.passive_slot); /* pslot →{heap} */
		combined = delta + READ_ONCE(pslot->delta);

		atomic_set(&g_node.state, CN_BUSY);

		/* Apply combined delta to the real counter. */
		pre = atomic_long_fetch_add(combined, &g_counter);

		/* Distribute result to passive. */
		WRITE_ONCE(pslot->result, (int)pre);
		smp_store_release(&pslot->result_ready.counter, 1);   /* WAKE passive */

		result = (long)(pre + pslot->delta);
		kfree(slot);   /* active doesn't need its slot anymore */
		return result;
	}

	kfree(slot);
	return result;
}

static int __init ct59_init(void)
{
	atomic_set(&g_node.state, CN_IDLE);
	g_node.passive_slot = NULL;

	/*
	 * Two "threads" each do a fetch-and-add of 10.
	 * In reality they'd race; here we simulate the combining protocol.
	 */
	ctree_fetch_add(10);
	ctree_fetch_add(10);

	pr_info("ct59: counter=%ld\n", atomic_long_read(&g_counter));
	return 0;
}

static void __exit ct59_exit(void)
{
	pr_info("ct59: final counter=%ld\n", atomic_long_read(&g_counter));
}

module_init(ct59_init);
module_exit(ct59_exit);
