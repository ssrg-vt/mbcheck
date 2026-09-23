// SPDX-License-Identifier: GPL-2.0
// 10_mpsc_list.c — Multi-producer single-consumer atomic linked list
//
// Non-blocking pattern: MPSC LOCK-FREE LIST (Moody-Camel / Michael variant).
//
// Multiple producers push nodes onto the head of a singly-linked list using
// a CAS on the global head pointer.  A single consumer drains the list by
// atomically swapping the head to NULL and then processing the captured chain.
//
// The push is lock-free (producers retry CAS on contention).
// The drain is wait-free for the consumer (single xchg).
//
// Pattern used in Linux kernel's llist (lock-less list) and in the kernel's
// call_rcu() deferred callback queue.
//
// Key APIs (kernel-memory-ordering-apis.md §2.4):
//   cmpxchg(&head, old, new)   — lock-free push (full barrier)
//   xchg(&head, NULL)          — atomic drain: swap head to NULL (full barrier)
//
// Ordering edges of interest for pointer analysis:
//   push:  kmalloc() → node            HeapObjVar
//          node->next = old_head        node->next →{heap|NULL}
//          cmpxchg(&g_head, ...)        g_head →{heap}
//   drain: xchg(&g_head, NULL) → chain  chain →{heap}
//          chain->next → ...            traversal of heap chain

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-10: MPSC lock-free list via cmpxchg push + xchg drain");

struct mpsc_node {
	int             val;
	struct mpsc_node *next;
};

/* List head: NULL means empty. */
static struct mpsc_node *g_head;

/* Lock-free push: returns 0 on success, -ENOMEM on alloc failure. */
static int mpsc_push(int val)
{
	struct mpsc_node *node, *old;

	node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
	if (!node)
		return -ENOMEM;

	node->val = val;

	/*
	 * Atomically prepend node to the list.  The CAS loop retries if
	 * another producer won the race to update the head.
	 */
	do {
		old        = READ_ONCE(g_head);          /* old →{heap|NULL} */
		node->next = old;                        /* node->next →{heap|NULL} */
	} while (cmpxchg(&g_head, old, node) != old);
	/* g_head →{heap} (new node) */
	return 0;
}

/* Atomic drain: returns the full list (or NULL if empty), resets head to NULL. */
static struct mpsc_node *mpsc_drain(void)
{
	/*
	 * xchg atomically replaces g_head with NULL and returns the old value.
	 * After this point g_head is NULL and we own the whole chain.
	 */
	return xchg(&g_head, (struct mpsc_node *)NULL);  /* chain →{heap|NULL} */
}

static int __init mpsc_init(void)
{
	mpsc_push(1);
	mpsc_push(2);
	mpsc_push(3);
	pr_info("mpsc: 3 nodes pushed\n");
	return 0;
}

static void __exit mpsc_exit(void)
{
	struct mpsc_node *chain, *next;

	chain = mpsc_drain();                        /* chain →{heap|NULL} */

	/* Walk and free the captured chain (single-threaded at this point). */
	while (chain) {                              /* chain →{heap} */
		next  = chain->next;                 /* next →{heap|NULL} */
		pr_info("mpsc: val=%d\n", chain->val);
		kfree(chain);
		chain = next;
	}
}

module_init(mpsc_init);
module_exit(mpsc_exit);
