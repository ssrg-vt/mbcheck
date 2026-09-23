// SPDX-License-Identifier: GPL-2.0
// 24_epoch_reclaim.c — Epoch-based memory reclamation (EBR)
//
// Non-blocking pattern: EPOCH-BASED RECLAMATION (Keir Fraser, 2004).
//
// EBR is a non-blocking memory reclamation scheme for lock-free data
// structures.  Three epochs cycle (0→1→2→0).  Each thread records the
// current epoch when it enters a critical section.  A retiring thread can
// safely free objects from epoch E-2 once all threads have passed through
// epoch E-1 (i.e., no thread is still in a critical section that observed
// the old object).
//
// Linux kernel analogue: RCU is a two-phase EBR (epoch 0 = current, epoch 1
// = after synchronize_rcu grace period).  The general EBR with three epochs
// is used in userspace lock-free libraries (Concurrency Kit, folly::rcu).
//
// This litmus shows:
//   - Global epoch counter (atomic_long_t)
//   - Per-"thread" (here: simulated with module init/exit) local epoch
//   - Retire list indexed by epoch
//   - Reclaim when safe epoch distance is reached
//
// Key APIs:
//   atomic_long_read / atomic_long_set       (§1.3)
//   atomic_long_cmpxchg                      (§1.3)
//   smp_store_release / smp_load_acquire     (§2.2)
//
// Ordering edges of interest:
//   retire: kmalloc() → obj →{heap}; stored in retire_list[epoch]
//   reclaim: epoch advance → smp_load_acquire(local_epoch) → safe to free

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-24: epoch-based reclamation (EBR) simulation");

#define NUM_EPOCHS 3

struct ebr_node {
	int              value;
	struct ebr_node *retire_next; /* free-list linkage */
};

/* Global epoch: threads read this to determine their current epoch. */
static atomic_long_t g_epoch = ATOMIC_LONG_INIT(0);

/* Per-"thread" local epoch (in a real system: per-CPU or per-task). */
static long g_local_epoch;

/* Retire lists: one per epoch.  Objects here are awaiting reclamation. */
static struct ebr_node *g_retire[NUM_EPOCHS];

/* Enter a critical section: pin the current epoch. */
static long ebr_enter(void)
{
	long e = atomic_long_read(&g_epoch);   /* relaxed: epoch is advisory */
	smp_store_release(&g_local_epoch, e); /* publish: I am in epoch e */
	barrier();                             /* compiler fence after pin */
	return e;
}

/* Leave critical section. */
static void ebr_leave(void)
{
	smp_store_release(&g_local_epoch, -1L); /* -1 = not in any CS */
}

/* Retire an object: link it onto the current epoch's free-list. */
static void ebr_retire(struct ebr_node *obj)
{
	long e = atomic_long_read(&g_epoch) % NUM_EPOCHS;

	obj->retire_next = g_retire[e];
	WRITE_ONCE(g_retire[e], obj);          /* g_retire[e] →{heap} */
}

/* Advance epoch and reclaim objects from the safe (oldest) epoch. */
static void ebr_quiesce(void)
{
	long old_e, new_e, safe_e;
	struct ebr_node *list, *next;

	/* Try to advance the global epoch (CAS). */
	old_e = atomic_long_read(&g_epoch);
	new_e = old_e + 1;
	atomic_long_cmpxchg(&g_epoch, old_e, new_e); /* full barrier */

	/*
	 * The "safe" epoch to reclaim from is two epochs back: objects
	 * retired in epoch (new_e - 2) cannot be referenced by any thread
	 * that observed epoch (new_e - 1) or newer.
	 */
	safe_e = ((new_e - 2) % NUM_EPOCHS + NUM_EPOCHS) % NUM_EPOCHS;

	/*
	 * Reclaim: drain the retire list for the safe epoch.
	 */
	list = xchg(&g_retire[safe_e], (struct ebr_node *)NULL); /* →{heap|NULL} */
	while (list) {                                            /* list →{heap} */
		next = list->retire_next;                         /* →{heap|NULL} */
		kfree(list);
		list = next;
	}
}

static int __init ebr_init_fn(void)
{
	struct ebr_node *obj;
	long e;

	/* Simulate a thread entering CS, allocating an object, retiring it. */
	e = ebr_enter();                           /* pin epoch */

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj) {
		ebr_leave();
		return -ENOMEM;
	}
	obj->value = 0x42;
	obj->retire_next = NULL;

	ebr_leave();                               /* leave CS */

	/* Retire the object into the current epoch's free-list. */
	ebr_retire(obj);                           /* g_retire[e] →{heap} */

	pr_info("ebr: obj retired in epoch %ld\n", e);
	return 0;
}

static void __exit ebr_exit_fn(void)
{
	/* Advance epoch twice and reclaim. */
	ebr_quiesce();
	ebr_quiesce();
	pr_info("ebr: reclamation done\n");
}

module_init(ebr_init_fn);
module_exit(ebr_exit_fn);
