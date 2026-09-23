// SPDX-License-Identifier: GPL-2.0
// 38_clh_lock.c — CLH queue-based spinlock (Craig, Landin & Hagersten 1993)
//
// Non-blocking pattern: CLH QUEUE LOCK (queue-based, cache-coherence friendly).
//
// The CLH lock is a queue-based spinlock where each thread spins on a locally-
// allocated node rather than a shared variable.  This eliminates cache-line
// bouncing under contention.
//
// Data structure:
//   - Each thread owns a "node" (on-stack or heap).
//   - The global tail (g_tail) points to the last waiting node.
//   - To lock: publish own node (locked=true), xchg(&g_tail, my_node) → pred
//             spin on pred->locked until false.
//   - To unlock: WRITE_ONCE(my_node->locked, false)  — release store.
//
// Ordering:
//   lock:   xchg(&g_tail, my_node)  — full barrier swap  (§2.4)
//           spin on smp_load_acquire(pred->locked)
//   unlock: smp_store_release(&my_node->locked, false)  — wakes successor
//
// Note: CLH is a *lock* (blocking if contended), but the lock itself is
// implemented non-blockingly (via a single xchg + spin on local memory).
// This litmus is included because the CLH lock structure involves heap-
// allocated nodes published through atomic pointer swaps — a rich source of
// pointer-analysis edges.
//
// Ordering edges of interest:
//   lock:   kmalloc() → my_node →{heap}; xchg(&g_tail, my_node) → g_tail →{heap}
//           spin on pred →{heap}->locked
//   unlock: smp_store_release(&my_node->locked, false) → successor wakes

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-38: CLH queue-based spinlock");

struct clh_node {
	bool locked;   /* true = predecessor still holds lock (or I am queued) */
};

/* Global tail: points to the last queued node. */
static struct clh_node *g_tail;

/* Per-"thread" state: my_node (published), pred (predecessor to spin on). */
struct clh_state {
	struct clh_node *my_node;   /* currently owned node */
	struct clh_node *pred;      /* predecessor's node (spin target) */
};

/* Acquire the CLH lock.  Returns predecessor node for release. */
static struct clh_node *clh_lock(struct clh_state *s)
{
	struct clh_node *my, *pred;

	my = kmalloc(sizeof(*my), GFP_KERNEL);     /* HeapObjVar */
	if (!my)
		return NULL;

	WRITE_ONCE(my->locked, true);              /* I am waiting */

	/*
	 * Atomically swing g_tail to my node and learn the predecessor.
	 * xchg has full-barrier semantics.
	 */
	pred = xchg(&g_tail, my);                  /* g_tail →{heap} (my) */
	                                            /* pred →{heap|NULL} */

	s->my_node = my;                           /* s->my_node →{heap} */
	s->pred    = pred;                         /* s->pred →{heap|NULL} */

	if (pred) {
		/*
		 * Spin until predecessor releases the lock.
		 * Acquire: predecessor's stores before unlock are visible here.
		 */
		while (smp_load_acquire(&pred->locked))
			cpu_relax();
	}

	return pred;  /* caller must kfree(pred) after use */
}

/* Release the CLH lock.  Successor (if any) is spinning on my_node->locked. */
static void clh_unlock(struct clh_state *s)
{
	/*
	 * Release store: all critical-section stores above are ordered before
	 * this.  The successor spinning on my_node->locked will observe false
	 * and enter the critical section.
	 */
	smp_store_release(&s->my_node->locked, false);  /* UNLOCK */
}

static int __init clh_init(void)
{
	struct clh_state s = {};
	struct clh_node  *old_pred;
	int counter = 0;

	/* Bootstrap: initial tail is a "released" dummy node. */
	g_tail = kmalloc(sizeof(*g_tail), GFP_KERNEL);  /* HeapObjVar */
	if (!g_tail)
		return -ENOMEM;
	WRITE_ONCE(g_tail->locked, false);

	/* Simulate two lock+unlock cycles. */
	old_pred = clh_lock(&s);                   /* s.my_node →{heap} */
	counter++;
	clh_unlock(&s);
	kfree(old_pred);

	old_pred = clh_lock(&s);
	counter++;
	clh_unlock(&s);
	kfree(old_pred);

	pr_info("clh: counter=%d\n", counter);
	return 0;
}

static void __exit clh_exit(void)
{
	struct clh_node *tail = READ_ONCE(g_tail); /* tail →{heap} */

	kfree(tail);
}

module_init(clh_init);
module_exit(clh_exit);
