// SPDX-License-Identifier: GPL-2.0
// 55_ofdq.c — Obstruction-free double-ended queue (Shafiei 2009)
//
// Non-blocking pattern: OBSTRUCTION-FREE DOUBLE-ENDED QUEUE (DEQUE).
//
// An obstruction-free deque allows push and pop from both ends.  Unlike a
// Treiber stack (single end) or MS-queue (FIFO), the deque has two active
// ends that any thread can access — no fixed producer/consumer assignment.
//
// This litmus implements a simplified version of Shafiei's OF deque using
// a doubly-linked list of heap nodes:
//   push_left / push_right: CAS on the left/right sentinel's link
//   pop_left / pop_right:   CAS to remove the leftmost/rightmost node
//
// Obstruction-freedom: a thread makes progress if it runs in isolation.
// Under contention it may have to retry (but no thread can block another
// indefinitely — unlike lock-based algorithms).
//
// Used in: Go's work-stealing scheduler (simplified OF deque per goroutine),
//          Java's ArrayDeque (lock-based, but OF variant in research).
//
// Ordering edges of interest:
//   push_right: kmalloc() → node →{heap}; node->prev = g_right_sentinel
//               node->next = NULL
//               cmpxchg(&g_right->next, NULL, node) → g_right->next →{heap}
//               smp_store_release(&g_right, node)   → g_right →{heap}
//   pop_left:   READ_ONCE(g_left->next) → node →{heap}
//               cmpxchg(&g_left->next, node, node->next)
//               kfree(old_left); g_left = node (new left sentinel)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-55: obstruction-free double-ended queue (Shafiei-style)");

struct dq_node {
	int             val;
	struct dq_node *prev;
	struct dq_node *next;
};

/*
 * The deque is represented as a doubly-linked list.
 * g_left  = leftmost sentinel (dummy); actual data starts at g_left->next.
 * g_right = rightmost sentinel (dummy); actual data ends at g_right->prev.
 */
static struct dq_node *g_left;
static struct dq_node *g_right;

static int dq_init(void)
{
	struct dq_node *ls, *rs;

	ls = kmalloc(sizeof(*ls), GFP_KERNEL);   /* left sentinel HeapObjVar */
	rs = kmalloc(sizeof(*rs), GFP_KERNEL);   /* right sentinel HeapObjVar */
	if (!ls || !rs) {
		kfree(ls);
		kfree(rs);
		return -ENOMEM;
	}

	ls->val  = -1; ls->prev = NULL; ls->next = rs;
	rs->val  = -1; rs->prev = ls;  rs->next = NULL;

	smp_store_release(&g_left,  ls);         /* g_left →{heap} */
	smp_store_release(&g_right, rs);         /* g_right →{heap} */
	return 0;
}

/* Push a value onto the right end. */
static bool dq_push_right(int val)
{
	struct dq_node *node, *right, *prev;

	node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
	if (!node)
		return false;
	node->val  = val;
	node->next = NULL;

	for (;;) {
		right       = smp_load_acquire(&g_right);  /* right →{heap} */
		prev        = READ_ONCE(right->prev);       /* prev →{heap} */
		node->prev  = right->prev;                  /* node->prev →{heap} */

		/* Link node to the left of the right sentinel. */
		if (cmpxchg(&right->prev, prev, node) == prev) {
			/* Also fix prev->next. */
			if (prev)
				WRITE_ONCE(prev->next, node);   /* prev->next →{heap} */
			return true;
		}
		/* Retry on CAS failure. */
	}
}

/* Push a value onto the left end. */
static bool dq_push_left(int val)
{
	struct dq_node *node, *left, *next;

	node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
	if (!node)
		return false;
	node->val  = val;
	node->prev = NULL;

	for (;;) {
		left        = smp_load_acquire(&g_left);   /* left →{heap} */
		next        = READ_ONCE(left->next);        /* next →{heap} */
		node->next  = next;                         /* node->next →{heap} */

		if (cmpxchg(&left->next, next, node) == next) {
			if (next)
				WRITE_ONCE(next->prev, node);   /* next->prev →{heap} */
			return true;
		}
	}
}

/* Pop a value from the left end.  Returns true and sets *val, or false if empty. */
static bool dq_pop_left(int *val)
{
	struct dq_node *left, *node, *next;

	for (;;) {
		left = smp_load_acquire(&g_left);   /* left →{heap} (sentinel) */
		node = READ_ONCE(left->next);       /* node →{heap|right_sentinel} */

		if (!node)
			return false;

		/* Check if this is the right sentinel (deque empty). */
		next = READ_ONCE(node->next);

		if (cmpxchg(&left->next, node, next) == node) {
			if (next)
				WRITE_ONCE(next->prev, left);
			*val = node->val;
			kfree(node);                    /* node was heap object */
			return true;
		}
	}
}

/* Pop a value from the right end. */
static bool dq_pop_right(int *val)
{
	struct dq_node *right, *node, *prev;

	for (;;) {
		right = smp_load_acquire(&g_right);  /* right →{heap} (sentinel) */
		node  = READ_ONCE(right->prev);      /* node →{heap|left_sentinel} */

		if (!node || node == smp_load_acquire(&g_left))
			return false;  /* empty */

		prev = READ_ONCE(node->prev);

		if (cmpxchg(&right->prev, node, prev) == node) {
			if (prev)
				WRITE_ONCE(prev->next, right);
			*val = node->val;
			kfree(node);                    /* node was heap object */
			return true;
		}
	}
}

static int __init dq_init_module(void)
{
	int v;

	if (dq_init() < 0)
		return -ENOMEM;

	/* Any thread can push/pop from either end — no fixed roles. */
	dq_push_right(10);
	dq_push_right(20);
	dq_push_left(5);
	dq_push_left(1);

	dq_pop_right(&v); pr_info("ofdq55: pop_right=%d\n", v);
	dq_pop_left(&v);  pr_info("ofdq55: pop_left=%d\n", v);

	pr_info("ofdq55: init complete\n");
	return 0;
}

static void __exit dq_exit_module(void)
{
	int v;

	/* Drain remaining items. */
	while (dq_pop_left(&v))
		pr_info("ofdq55: drain val=%d\n", v);

	/* Free sentinels. */
	kfree(g_left);
	kfree(g_right);
}

module_init(dq_init_module);
module_exit(dq_exit_module);
