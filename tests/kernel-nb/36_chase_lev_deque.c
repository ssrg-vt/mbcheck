// SPDX-License-Identifier: GPL-2.0
// 36_chase_lev_deque.c — Chase-Lev work-stealing deque (dynamic array)
//
// Non-blocking pattern: CHASE-LEV WORK-STEALING DEQUE (Chase & Lev, 2005).
//
// The Chase-Lev deque supports:
//   push_bottom / pop_bottom — by the OWNER thread (wait-free)
//   steal (pop_top)          — by THIEVES (lock-free)
//
// The deque is backed by a circular array.  The owner uses the bottom index
// without atomics; thieves use CAS on the top index.  The key ordering:
//   push: write slot, then fetch_add(bottom) — release semantics
//   steal: read top, read bottom, CAS(top): acquire on success
//
// Used in: Linux kernel's per-CPU work-queue (struct worker_pool deque),
//          Go runtime goroutine scheduler, Java ForkJoinPool.
//
// Key APIs:
//   atomic_long_read / atomic_long_set             (§1.3)
//   atomic_long_cmpxchg                            (§1.3)
//   smp_store_release / smp_load_acquire           (§2.2)
//
// Ordering edges of interest:
//   push:  kmalloc() → task; WRITE_ONCE(buf[b%CAP], task); fetch_add(bottom)
//          buf[b] →{heap}; bottom advanced with release
//   steal: load top (acquire); load bottom (acquire); buf[t%CAP] → task
//          CAS(top): task →{heap} on success

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-36: Chase-Lev work-stealing deque");

#define CL_CAP  8   /* must be power of two */

struct cl_task {
	int  id;
	int  cost;
};

/* Circular buffer: each slot holds a pointer to a heap-allocated task. */
static struct cl_task *g_buf[CL_CAP];
static atomic_long_t   g_top    = ATOMIC_LONG_INIT(0);  /* stolen from */
static long            g_bottom = 0;                    /* pushed to / popped from */

/* Owner push (wait-free). */
static bool cl_push(struct cl_task *task)
{
	long b = g_bottom;
	long t = atomic_long_read(&g_top);          /* relaxed: advisory */

	if (b - t >= CL_CAP)
		return false;  /* full */

	WRITE_ONCE(g_buf[b % CL_CAP], task);        /* buf[b] →{heap} */

	/* Release store: slot write visible before bottom advance. */
	smp_store_release(&g_bottom, b + 1);
	return true;
}

/* Owner pop_bottom (wait-free). */
static struct cl_task *cl_pop(void)
{
	long b, t;
	struct cl_task *task;

	b = g_bottom - 1;
	smp_store_release(&g_bottom, b);            /* tentatively shrink */

	t = atomic_long_read(&g_top);

	if (b < t) {
		/* Empty: undo. */
		smp_store_release(&g_bottom, t);
		return NULL;
	}

	task = READ_ONCE(g_buf[b % CL_CAP]);        /* task →{heap|NULL} */

	if (b > t)
		return task;  /* no race with thieves */

	/* b == t: race with a thief; use CAS. */
	if (atomic_long_cmpxchg(&g_top, t, t + 1) != t)
		task = NULL;  /* thief won */

	smp_store_release(&g_bottom, t + 1);
	return task;
}

/* Thief steal/pop_top (lock-free). */
static struct cl_task *cl_steal(void)
{
	long t, b;
	struct cl_task *task;

	t = atomic_long_read_acquire(&g_top);       /* acquire */
	smp_mb();
	b = smp_load_acquire(&g_bottom);            /* acquire */

	if (t >= b)
		return NULL;  /* empty */

	task = READ_ONCE(g_buf[t % CL_CAP]);        /* task →{heap|NULL} */

	/* CAS to claim the slot. */
	if (atomic_long_cmpxchg(&g_top, t, t + 1) != t)
		return NULL;  /* another thief won */

	return task;   /* task →{heap} */
}

static int __init cl_init(void)
{
	struct cl_task *t1, *t2, *stolen;

	t1 = kmalloc(sizeof(*t1), GFP_KERNEL);     /* HeapObjVar */
	t2 = kmalloc(sizeof(*t2), GFP_KERNEL);     /* HeapObjVar */
	if (!t1 || !t2) {
		kfree(t1); kfree(t2);
		return -ENOMEM;
	}
	t1->id = 1; t1->cost = 10;
	t2->id = 2; t2->cost = 20;

	cl_push(t1);                               /* buf[0] →{heap} */
	cl_push(t2);                               /* buf[1] →{heap} */

	stolen = cl_steal();                       /* stolen →{heap} */
	if (stolen)
		pr_info("cl: stolen id=%d\n", stolen->id);

	kfree(stolen);
	return 0;
}

static void __exit cl_exit(void)
{
	struct cl_task *t;

	while ((t = cl_pop()) != NULL) {           /* t →{heap} */
		pr_info("cl: popped id=%d\n", t->id);
		kfree(t);
	}
}

module_init(cl_init);
module_exit(cl_exit);
