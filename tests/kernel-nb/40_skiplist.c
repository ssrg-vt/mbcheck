// SPDX-License-Identifier: GPL-2.0
// 40_skiplist.c — Lock-free skip list (Harris-style mark-on-delete)
//
// Non-blocking pattern: LOCK-FREE SKIP LIST.
//
// A skip list with multiple levels of forward pointers.  Deletion is logical
// (mark the node with a "deleted" bit in its ->next pointer LSB) before the
// physical removal CAS.  This avoids the ABA problem on deletion.
//
// Used in:
//   - Linux kernel's kfree_rcu skip list (internal)
//   - Java's ConcurrentSkipListMap (Doug Lea)
//   - MemSQL / RocksDB index structures
//
// Simplified single-level litmus:
//   Insert: CAS on ->next to link a new node (ordered before its publication)
//   Delete: CAS to set LSB of ->next to 1 (logical delete)
//           then CAS to physically unlink (remove) the node
//   Lookup: traverse, skip logically-deleted nodes (CHECK LSB)
//
// Key APIs: cmpxchg on pointer-typed fields (§2.4), READ_ONCE (§2.1)
//
// Ordering edges of interest:
//   insert: kmalloc() → node →{heap}; CAS on prev->next → node
//   delete: CAS marks LSB(node->next) → node logically deleted
//           CAS on prev->next → skip node (physical unlink)
//   lookup: READ_ONCE(g_head->next) → node →{heap|NULL}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-40: lock-free skip list (single level, Harris mark-on-delete)");

#define SL_MARK ((uintptr_t)1)  /* deleted bit in LSB of pointer */

struct sl_node {
	int            key;
	int            value;
	struct sl_node *next;   /* LSB set = logically deleted */
};

/* Encode / decode the mark bit. */
static inline struct sl_node *sl_ptr(struct sl_node *p)
{
	return (struct sl_node *)((uintptr_t)p & ~SL_MARK);
}

static inline struct sl_node *sl_marked(struct sl_node *p)
{
	return (struct sl_node *)((uintptr_t)p | SL_MARK);
}

static inline bool sl_is_marked(struct sl_node *p)
{
	return (uintptr_t)p & SL_MARK;
}

/* Sentinel head (key = INT_MIN) and tail (key = INT_MAX). */
static struct sl_node *g_head;

/* Find predecessor and current for key.  Returns pred/curr (unmarked). */
static struct sl_node *sl_find_pred(int key, struct sl_node **curr_out)
{
	struct sl_node *pred, *curr, *next_raw, *next;

retry:
	pred = g_head;                                      /* pred →{heap} */
	curr = sl_ptr(READ_ONCE(pred->next));               /* curr →{heap|NULL} */

	while (curr && sl_ptr(curr) != NULL) {
		next_raw = READ_ONCE(curr->next);           /* next_raw →{heap|NULL} */
		next     = sl_ptr(next_raw);

		if (sl_is_marked(next_raw)) {
			/* Physically unlink curr (logically deleted node). */
			if (cmpxchg(&pred->next, curr, next) != curr)
				goto retry;
			kfree(curr);
			curr = next;                        /* curr →{heap|NULL} */
			continue;
		}

		if (curr->key >= key)
			break;

		pred = curr;                                /* pred →{heap} */
		curr = next;                                /* curr →{heap|NULL} */
	}

	*curr_out = curr;
	return pred;   /* pred →{heap} */
}

/* Insert a key-value pair. */
static int sl_insert(int key, int value)
{
	struct sl_node *node, *pred, *curr;

	node = kmalloc(sizeof(*node), GFP_KERNEL);          /* HeapObjVar */
	if (!node)
		return -ENOMEM;
	node->key   = key;
	node->value = value;

	for (;;) {
		pred = sl_find_pred(key, &curr);            /* pred →{heap} */

		if (curr && curr->key == key) {
			/* Already present. */
			kfree(node);
			return -EEXIST;
		}

		WRITE_ONCE(node->next, curr);               /* node->next →{heap|NULL} */

		/* Link node between pred and curr. */
		if (cmpxchg(&pred->next, curr, node) == curr)
			return 0;   /* g_head → ... → node →{heap} */
		/* CAS failed: retry. */
	}
}

/* Logical delete: mark LSB of node->next. */
static int sl_delete(int key)
{
	struct sl_node *pred, *curr, *next_raw, *next_marked;

	for (;;) {
		pred = sl_find_pred(key, &curr);            /* curr →{heap|NULL} */

		if (!curr || curr->key != key)
			return -ENOENT;

		next_raw    = READ_ONCE(curr->next);        /* next_raw →{heap|NULL} */
		next_marked = sl_marked(next_raw);

		/* Mark curr as logically deleted. */
		if (cmpxchg(&curr->next, next_raw, next_marked) == next_raw)
			break;  /* succeeded; physical removal by sl_find_pred */
	}
	return 0;
}

static int __init sl_init(void)
{
	/* Head sentinel: key = INT_MIN */
	g_head = kmalloc(sizeof(*g_head), GFP_KERNEL);     /* HeapObjVar */
	if (!g_head)
		return -ENOMEM;
	g_head->key  = INT_MIN;
	g_head->next = NULL;

	sl_insert(10, 100);
	sl_insert(20, 200);
	sl_insert(30, 300);

	sl_delete(20);

	pr_info("sl: 3 inserts, 1 delete\n");
	return 0;
}

static void __exit sl_exit(void)
{
	struct sl_node *p, *next;

	p = READ_ONCE(g_head);                              /* p →{heap} */
	while (p) {
		next = sl_ptr(READ_ONCE(p->next));          /* next →{heap|NULL} */
		kfree(p);
		p = next;
	}
}

module_init(sl_init);
module_exit(sl_exit);
