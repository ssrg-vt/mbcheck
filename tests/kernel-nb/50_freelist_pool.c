// SPDX-License-Identifier: GPL-2.0
// 50_freelist_pool.c — Lock-free object freelist (multi-producer multi-consumer)
//
// Non-blocking pattern: LOCK-FREE FREELIST / OBJECT POOL (MPMC).
//
// A freelist recycles fixed-size objects without calling kmalloc/kfree on
// every operation.  It is a Treiber stack (08) where *both* push (return) and
// pop (allocate) are performed by any thread — the "producer" for the freelist
// is the thread returning an object, and the "consumer" is the thread taking
// one.  Any thread can be either role at any time.
//
// This is different from a producer–consumer queue: the freelist has no
// ordering requirement on the objects themselves.  It is used in:
//   - Linux kernel's per-CPU slab allocator freelist
//   - folly::ThreadCachedArena page freelist
//   - io_uring's fixed-buffer freelist
//
// Protocol:
//   fl_get():  CAS(g_freelist, head, head->next) — take from top
//              Returns NULL if empty (caller must kmalloc).
//   fl_put():  set node->next = old_head; CAS(g_freelist, old_head, node)
//
// Ordering edges of interest:
//   fl_put:  kmalloc() → node →{heap}; node->next = old_head →{heap|NULL}
//            cmpxchg(&g_freelist, old_head, node) → g_freelist →{heap}
//   fl_get:  READ_ONCE(g_freelist) → head →{heap}
//            READ_ONCE(head->next) → next →{heap|NULL}
//            cmpxchg(&g_freelist, head, next) → g_freelist →{heap|NULL}
//            returned head →{heap} (reuse by caller)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-50: lock-free freelist object pool (MPMC recycle)");

struct fl_node {
	struct fl_node *next;   /* freelist linkage (when on freelist) */
	int             data;   /* user payload (when in use) */
};

/* The freelist head (Treiber stack). */
static struct fl_node *g_freelist;

/* Pre-populate the freelist with N objects. */
static int fl_populate(int n)
{
	int i;

	for (i = 0; i < n; i++) {
		struct fl_node *node, *old;

		node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
		if (!node)
			return -ENOMEM;
		node->data = 0;

		do {
			old        = READ_ONCE(g_freelist);  /* old →{heap|NULL} */
			node->next = old;
		} while (cmpxchg(&g_freelist, old, node) != old);
		/* g_freelist →{heap} (node) */
	}
	return 0;
}

/* Allocate one object from the freelist (pop). */
static struct fl_node *fl_get(void)
{
	struct fl_node *head, *next;

	do {
		head = READ_ONCE(g_freelist);        /* head →{heap|NULL} */
		if (!head)
			return NULL;
		next = READ_ONCE(head->next);        /* next →{heap|NULL} */
	} while (cmpxchg(&g_freelist, head, next) != head);
	/* g_freelist →{heap|NULL} (next); head is now owned by caller */
	head->next = NULL;
	return head;   /* head →{heap} */
}

/* Return one object to the freelist (push). */
static void fl_put(struct fl_node *node)
{
	struct fl_node *old;

	node->data = 0;  /* scrub */

	do {
		old        = READ_ONCE(g_freelist);  /* old →{heap|NULL} */
		node->next = old;
	} while (cmpxchg(&g_freelist, old, node) != old);
	/* g_freelist →{heap} (node) */
}

static int __init fl_init(void)
{
	struct fl_node *a, *b;

	/* Pre-populate freelist with 4 objects. */
	if (fl_populate(4) < 0)
		return -ENOMEM;

	/* Thread 1 (allocator): get two objects and use them. */
	a = fl_get();       /* a →{heap} */
	b = fl_get();       /* b →{heap} */

	if (a) a->data = 42;
	if (b) b->data = 99;

	pr_info("fl50: got a=%d b=%d\n", a ? a->data : -1, b ? b->data : -1);

	/* Thread 2 (recycler): return them. */
	if (a) fl_put(a);  /* a back on freelist */
	if (b) fl_put(b);  /* b back on freelist */

	pr_info("fl50: init complete\n");
	return 0;
}

static void __exit fl_exit(void)
{
	struct fl_node *node;

	/* Drain the freelist and free all memory. */
	while ((node = fl_get()) != NULL) {   /* node →{heap} */
		kfree(node);
	}
}

module_init(fl_init);
module_exit(fl_exit);
