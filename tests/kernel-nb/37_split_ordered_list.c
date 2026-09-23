// SPDX-License-Identifier: GPL-2.0
// 37_split_ordered_list.c — Split-ordered lock-free hash table (Shalev & Shavit 2006)
//
// Non-blocking pattern: SPLIT-ORDERED LIST (lock-free hash table).
//
// A split-ordered list is a lock-free hash table built on a single sorted
// linked list where keys are bit-reversed.  Resizing is incremental:
// new buckets point into the existing sorted list.
//
// The key insight: each bucket pointer is a sentinel node in the sorted list.
// Lookup/insert/delete use the standard Treiber-like CAS on ->next pointers.
// Bucket initialisation lazily installs sentinel nodes on first access.
//
// This litmus shows:
//   - Bucket array: g_buckets[i] points to sentinel (heap node)
//   - Node allocation + CAS-insert into the list
//   - Bucket lazy-init via cmpxchg (install sentinel)
//
// Key APIs: cmpxchg (§2.4), READ_ONCE/WRITE_ONCE (§2.1)
//
// Ordering edges of interest:
//   insert: kmalloc() → node →{heap}; node linked via CAS on prev->next
//           g_buckets[hash] → sentinel →{heap}
//   lookup: g_buckets[hash] → sentinel → ... → node →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-37: split-ordered lock-free hash table (Shalev-Shavit)");

#define SO_BUCKETS  4

/* Bit-reverse a key for split-ordered positioning. */
static unsigned int so_reverse(unsigned int key)
{
	unsigned int r = 0;
	int i;
	for (i = 0; i < 32; i++, key >>= 1)
		r = (r << 1) | (key & 1);
	return r;
}

struct so_node {
	unsigned int   so_key;   /* bit-reversed key; 0x00... = sentinel */
	int            value;
	struct so_node *next;
	bool           sentinel; /* true if this is a bucket sentinel */
};

/* The global sorted list (head is sentinel for bucket 0). */
static struct so_node *g_list_head;

/* Bucket array: each entry points to its sentinel node in the list. */
static struct so_node *g_buckets[SO_BUCKETS];

/* Allocate a sentinel for bucket idx. */
static struct so_node *so_make_sentinel(unsigned int idx)
{
	struct so_node *s;

	s = kmalloc(sizeof(*s), GFP_KERNEL);   /* HeapObjVar (sentinel) */
	if (!s)
		return NULL;
	s->so_key  = so_reverse(idx) & ~1U;   /* clear LSB → sentinel marker */
	s->value   = 0;
	s->next    = NULL;
	s->sentinel = true;
	return s;
}

/* Ensure bucket idx has a sentinel (lazy initialisation via cmpxchg). */
static struct so_node *so_init_bucket(unsigned int idx)
{
	struct so_node *existing, *sentinel;

	existing = smp_load_acquire(&g_buckets[idx]);  /* existing →{heap|NULL} */
	if (existing)
		return existing;

	sentinel = so_make_sentinel(idx);              /* HeapObjVar */
	if (!sentinel)
		return NULL;

	/* CAS: install the sentinel; if another CPU beat us, free ours. */
	existing = cmpxchg(&g_buckets[idx],
			   (struct so_node *)NULL, sentinel);
	if (existing) {
		kfree(sentinel);           /* lost the race */
		return existing;           /* existing →{heap} */
	}

	/*
	 * We won: link sentinel into the sorted list (simplified: prepend).
	 * In a real SO list this would be a sorted insert.
	 */
	struct so_node *old_head;
	do {
		old_head       = READ_ONCE(g_list_head);   /* old_head →{heap|NULL} */
		sentinel->next = old_head;
	} while (cmpxchg(&g_list_head, old_head, sentinel) != old_head);

	/* g_list_head →{heap} (sentinel), g_buckets[idx] →{heap} (sentinel) */
	return sentinel;
}

/* Insert a key-value pair. */
static int so_insert(unsigned int key, int value)
{
	unsigned int  bucket = key % SO_BUCKETS;
	struct so_node *sentinel, *node, *curr;

	sentinel = so_init_bucket(bucket);             /* sentinel →{heap} */
	if (!sentinel)
		return -ENOMEM;

	node = kmalloc(sizeof(*node), GFP_KERNEL);     /* HeapObjVar (data) */
	if (!node)
		return -ENOMEM;
	node->so_key  = so_reverse(key) | 1U;         /* set LSB → data marker */
	node->value   = value;
	node->sentinel = false;

	/* Simplified sorted insert after sentinel. */
	do {
		curr        = READ_ONCE(sentinel->next);   /* curr →{heap|NULL} */
		node->next  = curr;
	} while (cmpxchg(&sentinel->next, curr, node) != curr);

	/* sentinel->next →{heap} (node), node →{heap} */
	return 0;
}

static int __init so_init(void)
{
	struct so_node *h;

	/* Initialise the list with bucket-0 sentinel as head. */
	h = so_make_sentinel(0);                       /* HeapObjVar */
	if (!h)
		return -ENOMEM;
	g_list_head  = h;                              /* g_list_head →{heap} */
	g_buckets[0] = h;                             /* g_buckets[0] →{heap} */

	so_insert(1, 100);
	so_insert(5, 500);
	so_insert(3, 300);
	pr_info("so: 3 items inserted\n");
	return 0;
}

static void __exit so_exit(void)
{
	struct so_node *p, *next;

	/* Walk and free the sorted list. */
	p = READ_ONCE(g_list_head);                    /* p →{heap|NULL} */
	while (p) {                                    /* p →{heap} */
		next = p->next;                        /* next →{heap|NULL} */
		kfree(p);
		p = next;
	}
}

module_init(so_init);
module_exit(so_exit);
