// SPDX-License-Identifier: GPL-2.0
// 08_stack_treiber.c — Lock-free Treiber stack (push/pop via cmpxchg)
//
// Non-blocking pattern: TREIBER STACK (lock-free, wait-free push).
//
// The Treiber stack is the canonical lock-free stack.  It maintains a global
// head pointer and uses compare-and-swap to:
//   Push: set node->next = old_head, then CAS(head, old_head, node)
//   Pop:  read head, set new_head = head->next, CAS(head, head, new_head)
//
// The push is wait-free for a single producer (only one CAS needed).
// The pop is lock-free (may retry if another thread modifies head).
//
// Key API (kernel-memory-ordering-apis.md §2.4):
//   cmpxchg(ptr, old, new) — full barrier CAS on 8-byte pointer
//
// ABA note: a real Treiber stack needs ABA protection (generation counter
// or hazard pointers).  This litmus test omits that for clarity.
//
// Ordering edges of interest for pointer analysis:
//   push: kmalloc() → node           HeapObjVar
//         node->next = g_head        node->next →{heap or NULL}
//         cmpxchg(&g_head, ...)      g_head →{heap} after CAS
//   pop:  g_head → old              old →{heap}
//         old->next → g_head        g_head →{heap->next} after CAS

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-08: lock-free Treiber stack via cmpxchg on head pointer");

struct treiber_node {
	int               val;
	struct treiber_node *next;
};

/* Stack head: NULL means empty. */
static struct treiber_node *g_head;

/* Push val onto the Treiber stack.  Returns 0 on success, -ENOMEM on alloc fail. */
static int treiber_push(int val)
{
	struct treiber_node *node, *old_head;

	node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
	if (!node)
		return -ENOMEM;

	node->val = val;

	/*
	 * Atomically swing head from old_head to node.
	 * cmpxchg has full barrier semantics; node->next is written before
	 * the CAS so any reader that sees the new head also sees next.
	 */
	do {
		old_head   = READ_ONCE(g_head);           /* old_head →{heap|NULL} */
		node->next = old_head;                    /* node->next →{heap|NULL} */
	} while (cmpxchg(&g_head, old_head, node) != old_head);
	/* After CAS: g_head →{heap} (newly allocated node) */
	return 0;
}

/* Pop one node.  Returns pointer or NULL if empty. */
static struct treiber_node *treiber_pop(void)
{
	struct treiber_node *old, *next;

	do {
		old = READ_ONCE(g_head);                  /* old →{heap|NULL} */
		if (!old)
			return NULL;
		next = old->next;                         /* next →{heap|NULL} */
	} while (cmpxchg(&g_head, old, next) != old);
	/* After CAS: g_head →{heap->next} */
	return old;                                   /* caller frees */
}

static int __init treiber_init(void)
{
	treiber_push(10);
	treiber_push(20);
	treiber_push(30);
	pr_info("treiber: pushed 3 items\n");
	return 0;
}

static void __exit treiber_exit(void)
{
	struct treiber_node *n;

	while ((n = treiber_pop()) != NULL) {        /* n →{heap} */
		pr_info("treiber: popped val=%d\n", n->val);
		kfree(n);
	}
}

module_init(treiber_init);
module_exit(treiber_exit);
