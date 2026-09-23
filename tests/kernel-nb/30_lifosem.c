// SPDX-License-Identifier: GPL-2.0
// 30_lifosem.c — LIFO semaphore (stack-based, cache-friendly wakeup order)
//
// Non-blocking pattern: LIFO SEMAPHORE (folly::LifoSem).
//
// A LIFO semaphore prefers to wake the most recently sleeping thread,
// improving cache reuse.  The value is stored in an atomic counter:
//   post:  if (no waiters) atomic_inc(); else wake top-of-stack waiter
//   wait:  if (count > 0) atomic_dec(); else push self on waiter stack + sleep
//
// The waiter stack is a lock-free Treiber stack of wait nodes; each node
// contains a baton (one-shot flag) and a pointer to the next waiter.
//
// This litmus models the lock-free publish and acquire on the wait node:
//   post: smp_store_release(&node->ready, 1)  — baton pass
//   wait: spin on smp_load_acquire(&node->ready)
//
// Kernel analogue: the kernel's up()/down() semaphore is FIFO.  LIFO order
// is used in the per-CPU work queue's idle worker wakeup (try_to_wake_up
// selects the last sleeping worker first for cache affinity).
//
// Ordering edges of interest:
//   post: kmalloc → node →{heap}; stacked; smp_store_release(ready=1)
//   wait: READ_ONCE(g_top) → node →{heap}; smp_load_acquire(ready)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-30: LIFO semaphore via lock-free Treiber stack of wait-nodes");

struct lifosem_node {
	atomic_t            ready;   /* 0=sleeping, 1=woken */
	struct lifosem_node *next;   /* next waiter on the stack */
};

static atomic_t              g_count = ATOMIC_INIT(0);
static struct lifosem_node  *g_top;   /* waiter stack head */

/* Post: increment count or wake the top waiter. */
static void lifosem_post(void)
{
	struct lifosem_node *top, *next;

	/* Try to pop a waiter and wake it directly. */
	do {
		top = READ_ONCE(g_top);              /* top →{heap|NULL} */
		if (!top) {
			/* No waiters: just increment the counter. */
			atomic_inc(&g_count);
			return;
		}
		next = top->next;                    /* next →{heap|NULL} */
	} while (cmpxchg(&g_top, top, next) != top);

	/*
	 * We popped `top`; wake the sleeping thread by setting its ready flag.
	 * Release store: all prior stores (counter update etc.) are ordered
	 * before the wakeup.
	 */
	atomic_set_release(&top->ready, 1);      /* BATON PASS */
}

/* Wait: decrement count if positive, else push self on stack and spin. */
static void lifosem_wait(struct lifosem_node *node)
{
	int old;

	/* Fast path: decrement if count > 0. */
	do {
		old = atomic_read(&g_count);
		if (old <= 0)
			break;
	} while (atomic_cmpxchg(&g_count, old, old - 1) != old);

	if (old > 0)
		return;  /* decremented successfully — no need to block */

	/* Slow path: push ourselves onto the waiter stack. */
	atomic_set(&node->ready, 0);

	struct lifosem_node *top;
	do {
		top        = READ_ONCE(g_top);       /* top →{heap|NULL} */
		node->next = top;
	} while (cmpxchg(&g_top, top, node) != top);
	/* g_top →{heap} (node) */

	/* Spin until poster sets our ready flag. */
	while (!atomic_read_acquire(&node->ready))
		cpu_relax();
}

static int __init lifosem_init(void)
{
	struct lifosem_node *wnode;

	wnode = kmalloc(sizeof(*wnode), GFP_KERNEL);  /* HeapObjVar */
	if (!wnode)
		return -ENOMEM;
	wnode->next = NULL;

	/* Post two tokens, then drain one via wait. */
	lifosem_post();
	lifosem_post();

	lifosem_wait(wnode);   /* should consume without blocking */
	pr_info("lifosem: wait returned (count now %d)\n",
		atomic_read(&g_count));

	kfree(wnode);
	return 0;
}

static void __exit lifosem_exit(void)
{
	pr_info("lifosem: final count=%d\n", atomic_read(&g_count));
}

module_init(lifosem_init);
module_exit(lifosem_exit);
