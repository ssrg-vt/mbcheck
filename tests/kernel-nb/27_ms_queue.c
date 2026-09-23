// SPDX-License-Identifier: GPL-2.0
// 27_ms_queue.c — Michael-Scott non-blocking MPMC queue (Michael & Scott 1996)
//
// Non-blocking pattern: MICHAEL-SCOTT QUEUE (lock-free, linearizable FIFO).
//
// The Michael-Scott queue is the canonical lock-free MPMC queue.  It uses
// a singly-linked list with a sentinel head node.  Enqueue appends to the
// tail; dequeue removes from the head.  Both operations use CAS.
//
// Key invariant: g_tail always points to the last node OR the last node's
// next (if a concurrent enqueuer has linked but not yet swung tail).
//
// Enqueue protocol:
//   1. Allocate node, set node->next = NULL
//   2. Loop: read tail, read tail->next
//      - If tail->next == NULL: CAS(tail->next, NULL, node) → success
//      - Else: help advance tail: CAS(g_tail, tail, tail->next)
//   3. Advance g_tail to node (best-effort)
//
// Dequeue protocol:
//   1. Loop: read head, read tail, read head->next
//      - If head == tail and head->next == NULL: empty
//      - If head == tail and head->next != NULL: help tail advance
//      - Else: CAS(g_head, head, head->next); return head->next->value
//
// Key APIs: cmpxchg (§2.4), READ_ONCE, smp_mb (§2.3)
//
// Ordering edges of interest:
//   enqueue: kmalloc() → node →{heap}; node linked via CAS on tail->next
//            g_tail swung to node
//   dequeue: g_head → sentinel → next →{heap} (value node)
//            CAS(g_head, sentinel, next)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-27: Michael-Scott lock-free MPMC queue");

struct ms_node {
	int            val;
	struct ms_node *next;  /* NULL = end of queue */
};

/* Queue: head = sentinel (dummy), tail = last real or sentinel node. */
static struct ms_node *g_head;
static struct ms_node *g_tail;

/* Enqueue value. Returns 0 on success, -ENOMEM on alloc failure. */
static int ms_enqueue(int val)
{
	struct ms_node *node, *tail, *next;

	node = kmalloc(sizeof(*node), GFP_KERNEL);  /* HeapObjVar */
	if (!node)
		return -ENOMEM;
	node->val  = val;
	WRITE_ONCE(node->next, NULL);

	for (;;) {
		tail = READ_ONCE(g_tail);                  /* tail →{heap} */
		next = READ_ONCE(tail->next);              /* next →{heap|NULL} */

		if (tail != READ_ONCE(g_tail))
			continue;  /* tail moved under us */

		if (!next) {
			/* Tail->next is NULL: try to link our node. */
			if (cmpxchg(&tail->next, (struct ms_node *)NULL, node)
					== NULL) {
				/* Swing tail (best-effort: OK if it fails). */
				cmpxchg(&g_tail, tail, node);
				return 0;
			}
		} else {
			/* Tail not pointing to last node; help advance it. */
			cmpxchg(&g_tail, tail, next);
		}
	}
}

/* Dequeue. Returns value or -1 if empty. */
static int ms_dequeue(void)
{
	struct ms_node *head, *tail, *next;
	int val;

	for (;;) {
		head = READ_ONCE(g_head);                  /* head →{heap} */
		tail = READ_ONCE(g_tail);                  /* tail →{heap} */
		next = READ_ONCE(head->next);              /* next →{heap|NULL} */

		if (head != READ_ONCE(g_head))
			continue;

		if (head == tail) {
			if (!next)
				return -1;  /* empty */
			/* Tail lagging; help advance. */
			cmpxchg(&g_tail, tail, next);
			continue;
		}

		/* Read value from next node (the real node; head is sentinel). */
		val = next->val;                           /* next →{heap} */

		/* Swing head to next (next becomes new sentinel). */
		if (cmpxchg(&g_head, head, next) == head) {
			kfree(head);                       /* head was old sentinel */
			return val;
		}
	}
}

static int __init ms_init(void)
{
	struct ms_node *sentinel;

	/* Sentinel node: head and tail both point here initially. */
	sentinel = kmalloc(sizeof(*sentinel), GFP_KERNEL);  /* HeapObjVar */
	if (!sentinel)
		return -ENOMEM;
	sentinel->val  = 0;
	sentinel->next = NULL;
	g_head = sentinel;                              /* g_head →{heap} */
	g_tail = sentinel;                              /* g_tail →{heap} */

	ms_enqueue(10);
	ms_enqueue(20);
	ms_enqueue(30);
	pr_info("ms_queue: enqueued 3 items\n");
	return 0;
}

static void __exit ms_exit(void)
{
	int v;

	while ((v = ms_dequeue()) != -1)               /* dequeued →{heap} freed */
		pr_info("ms_queue: dequeued %d\n", v);

	kfree(g_head);  /* free final sentinel */
}

module_init(ms_init);
module_exit(ms_exit);
