// SPDX-License-Identifier: GPL-2.0
// 39_mcs_lock.c — MCS queue-based spinlock (Mellor-Crummey & Scott 1991)
//
// Non-blocking pattern: MCS QUEUE LOCK.
//
// The MCS lock is the canonical queue-based spinlock.  Unlike CLH, each thread
// spins on its *own* node rather than the predecessor's.  This is better for
// NUMA because the node lives in the waiting thread's memory.
//
// Protocol:
//   Acquire: xchg(&g_tail, &my_node) → pred
//            if pred != NULL: pred->next = &my_node; spin(my_node.locked)
//   Release: if CAS(&g_tail, &my_node, NULL) fails:
//              spin until my_node.next != NULL
//              smp_store_release(&my_node.next->locked, 0)  → wake successor
//
// Linux uses an equivalent structure in the qspinlock (osq_lock) and in the
// MCS-based kernel mutex (mutex_lock_slowpath).
//
// Ordering edges of interest:
//   acquire: xchg(&g_tail, my_node) → g_tail →{heap} (my_node)
//            pred →{heap}; smp_store_release(&pred->next, my_node)
//            spin on smp_load_acquire(&my_node->locked)
//   release: cmpxchg(&g_tail, my_node, NULL) → unlock if no successor
//            or smp_store_release(&next->locked, 0)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-39: MCS queue-based spinlock");

struct mcs_node {
	struct mcs_node *next;   /* successor in queue */
	int              locked; /* 1 = waiting, 0 = lock granted */
};

/* Global queue tail. */
static struct mcs_node *g_tail;

/* Acquire the MCS lock. */
static void mcs_lock(struct mcs_node *my)
{
	struct mcs_node *pred;

	WRITE_ONCE(my->locked, 1);
	WRITE_ONCE(my->next,   NULL);

	/*
	 * Atomically enqueue self, learn predecessor.
	 * xchg provides a full barrier.
	 */
	pred = xchg(&g_tail, my);                   /* g_tail →{heap} (my) */
	                                             /* pred →{heap|NULL} */
	if (!pred)
		return;  /* uncontended — we hold the lock */

	/*
	 * Tell predecessor where to find us (so it can wake us).
	 * Release store: our locked=1 write is visible before this.
	 */
	smp_store_release(&pred->next, my);          /* pred->next →{heap} (my) */

	/* Spin on our own locked flag (acquire). */
	while (smp_load_acquire(&my->locked))
		cpu_relax();
}

/* Release the MCS lock. */
static void mcs_unlock(struct mcs_node *my)
{
	struct mcs_node *next;

	next = smp_load_acquire(&my->next);          /* next →{heap|NULL} */

	if (!next) {
		/*
		 * No known successor.  Try to reset the tail to NULL.
		 * If CAS succeeds: no successor races; we are done.
		 */
		if (cmpxchg(&g_tail, my, (struct mcs_node *)NULL) == my)
			return;

		/* A successor is in the middle of enqueueing; spin for next. */
		do {
			next = smp_load_acquire(&my->next);  /* next →{heap} */
		} while (!next);
	}

	/* Wake successor: release store on its locked flag. */
	smp_store_release(&next->locked, 0);         /* UNLOCK successor */
}

static int __init mcs_init(void)
{
	struct mcs_node *n1, *n2;
	int counter = 0;

	n1 = kmalloc(sizeof(*n1), GFP_KERNEL);       /* HeapObjVar */
	n2 = kmalloc(sizeof(*n2), GFP_KERNEL);       /* HeapObjVar */
	if (!n1 || !n2) {
		kfree(n1); kfree(n2);
		return -ENOMEM;
	}

	/* First lock+unlock. */
	mcs_lock(n1);                                /* g_tail →{heap} n1 */
	counter++;
	mcs_unlock(n1);                              /* g_tail = NULL */

	/* Second lock+unlock. */
	mcs_lock(n2);
	counter++;
	mcs_unlock(n2);

	pr_info("mcs: counter=%d\n", counter);

	kfree(n1);
	kfree(n2);
	return 0;
}

static void __exit mcs_exit(void)
{
	pr_info("mcs: module unloaded\n");
}

module_init(mcs_init);
module_exit(mcs_exit);
