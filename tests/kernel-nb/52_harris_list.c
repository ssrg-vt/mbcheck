// SPDX-License-Identifier: GPL-2.0
// 52_harris_list.c — Harris lock-free linked list with logical deletion
//
// Non-blocking pattern: HARRIS LOCK-FREE LINKED LIST (Harris 2001).
//
// The Harris list supports concurrent insert, delete, and search from
// *multiple threads simultaneously* — no thread plays a fixed producer or
// consumer role.  Logical deletion uses the LSB of the next pointer as a
// mark bit (the same trick as the skiplist in litmus 40, but here applied
// to a standalone singly-linked list with sorted keys).
//
// Operations:
//   search(key): traverse until key found; help physically remove marked nodes
//   insert(key): find window (pred, curr); CAS(pred->next, curr, new_node)
//   delete(key): find node; CAS-mark(node->next, unmarked, marked);
//                CAS(pred->next, node, node->next) — physical remove
//
// The key differentiator from 40_skiplist is that *any* thread can call any
// of insert/delete/search at any time — the pattern is N-way peer concurrent
// access, not a producer pushing and a consumer popping.
//
// Ordering edges of interest:
//   insert: kmalloc() → node →{heap}; node->key = k; node->next = curr
//           cmpxchg(&pred->next, curr, node) → pred->next →{heap}
//   delete: cmpxchg(&node->next, next, MARK(next)) — logical delete
//           cmpxchg(&pred->next, node, next) — physical unlink
//   search: READ_ONCE(head->next) → curr →{heap}; traverse curr→next chain

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-52: Harris lock-free linked list with logical deletion");

/* LSB of next pointer = mark (logically deleted). */
#define HL_MARK(p)       ((struct hl_node *)((unsigned long)(p) | 1UL))
#define HL_UNMARK(p)     ((struct hl_node *)((unsigned long)(p) & ~1UL))
#define HL_IS_MARKED(p)  ((unsigned long)(p) & 1UL)

struct hl_node {
	int             key;
	struct hl_node *next;   /* LSB = mark */
};

/* Sentinel head (key = INT_MIN) and tail (key = INT_MAX). */
static struct hl_node g_head;
static struct hl_node g_tail;

/* Find the window [pred, curr] for key k; physically removes marked nodes. */
static bool hl_find(int key, struct hl_node **pred_out,
		    struct hl_node **curr_out)
{
	struct hl_node *pred, *curr, *next;

retry:
	pred = &g_head;
	curr = READ_ONCE(pred->next);          /* curr →{heap} */

	while (curr != &g_tail) {
		next = READ_ONCE(curr->next);      /* next →{heap|marked} */

		if (HL_IS_MARKED(next)) {
			/* Physically remove logically-deleted node. */
			if (cmpxchg(&pred->next, curr, HL_UNMARK(next)) != curr)
				goto retry;
			curr = HL_UNMARK(next);
		} else {
			if (curr->key >= key) {
				*pred_out = pred;
				*curr_out = curr;
				return curr->key == key;
			}
			pred = curr;
			curr = next;
		}
	}
	*pred_out = pred;
	*curr_out = curr;
	return false;
}

/* Insert key k.  Returns true if inserted, false if already present. */
static bool hl_insert(int key)
{
	struct hl_node *pred, *curr, *node;

	node = kmalloc(sizeof(*node), GFP_KERNEL);   /* HeapObjVar */
	if (!node)
		return false;

	node->key = key;

	for (;;) {
		if (hl_find(key, &pred, &curr)) {
			kfree(node);
			return false;  /* duplicate */
		}
		node->next = curr;                          /* node->next →{heap} */
		if (cmpxchg(&pred->next, curr, node) == curr)
			return true;   /* inserted; pred->next →{heap} (node) */
		/* CAS failed: retry */
	}
}

/* Delete key k.  Returns true if deleted, false if not found. */
static bool hl_delete(int key)
{
	struct hl_node *pred, *curr, *next;

	for (;;) {
		if (!hl_find(key, &pred, &curr))
			return false;  /* not found */

		next = READ_ONCE(curr->next);               /* next →{heap} */

		/* Logical delete: mark next pointer. */
		if (cmpxchg(&curr->next, next, HL_MARK(next)) != next)
			continue;  /* retry */

		/* Physical unlink (best-effort; hl_find will clean up on next call). */
		cmpxchg(&pred->next, curr, next);
		kfree(curr);
		return true;
	}
}

static int __init hl_init(void)
{
	/* Initialise sentinels. */
	g_head.key  = -2147483648;  /* INT_MIN */
	g_tail.key  =  2147483647;  /* INT_MAX */
	g_head.next = &g_tail;
	g_tail.next = NULL;

	/* Multiple "thread" roles: all call insert/delete/search. */
	hl_insert(10);
	hl_insert(20);
	hl_insert(30);
	hl_insert(15);
	hl_delete(20);
	hl_insert(25);

	pr_info("hl52: init complete\n");
	return 0;
}

static void __exit hl_exit(void)
{
	struct hl_node *curr, *next;

	/* Drain the list. */
	curr = HL_UNMARK(READ_ONCE(g_head.next));   /* curr →{heap} */
	while (curr != &g_tail) {
		next = HL_UNMARK(READ_ONCE(curr->next));
		kfree(curr);
		curr = next;
	}
}

module_init(hl_init);
module_exit(hl_exit);
