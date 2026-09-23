// SPDX-License-Identifier: GPL-2.0
// 44_indexed_mempool.c — Lock-free indexed memory pool / freelist
//
// Non-blocking pattern: LOCK-FREE INDEXED FREELIST (ABA-safe).
//
// A pre-allocated slab of objects, recycled through a lock-free freelist.
// Using integer *indices* (instead of raw pointers) avoids the ABA problem:
// the index is unique per slot, so an old value cannot compare equal to a
// reused pointer.
//
// Protocol:
//   Alloc: read head index; CAS(&g_head, head, next[head])
//   Free:  my_node->next = head; CAS(&g_head, head, my_idx)
//
// This is the pattern used in:
//   - lock-free memory pools in real-time kernels (Xenomai, PREEMPT-RT)
//   - io_uring fixed-buffer freelist
//   - eBPF map free-list management
//
// Key APIs: cmpxchg on u32 index (§2.4), READ_ONCE (§2.1)
//
// Ordering edges of interest:
//   alloc: READ_ONCE(g_head) → head_idx; g_pool[head_idx] →{slab}
//          cmpxchg(&g_head, head_idx, g_pool[head_idx].next_idx) → success
//          return &g_pool[head_idx]  →{slab}
//   free:  WRITE_ONCE(g_pool[idx].next_idx, g_head); cmpxchg(&g_head, ...)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-44: lock-free indexed memory pool / freelist");

#define POOL_NONE  0xFFFFFFFFU   /* sentinel: empty */
#define POOL_CAP   8

struct pool_node {
	u32  next_idx;   /* freelist next (index, not pointer) */
	int  data;
};

/* Pre-allocated slab.  Each slot is either free (on g_freelist) or in use. */
static struct pool_node g_pool[POOL_CAP];

/* Head of the freelist: an index into g_pool, or POOL_NONE. */
static u32 g_head;

/* Initialise the freelist: link all slots. */
static void pool_init(void)
{
	u32 i;

	for (i = 0; i < POOL_CAP - 1; i++)
		g_pool[i].next_idx = i + 1;
	g_pool[POOL_CAP - 1].next_idx = POOL_NONE;

	smp_store_release(&g_head, 0);   /* publish the freelist head */
}

/* Allocate one node from the pool (lock-free). */
static struct pool_node *pool_alloc(void)
{
	u32 head, next;

	do {
		head = smp_load_acquire(&g_head);          /* head: current head idx */
		if (head == POOL_NONE)
			return NULL;   /* pool empty */

		next = READ_ONCE(g_pool[head].next_idx);   /* next: successor idx */
	} while (cmpxchg(&g_head, head, next) != head);

	/* g_pool[head] →{slab} — exclusively owned by caller */
	return &g_pool[head];   /* →{slab} */
}

/* Return a node to the pool (lock-free). */
static void pool_free(struct pool_node *node)
{
	u32 my_idx = (u32)(node - g_pool);   /* compute index from pointer */
	u32 head;

	do {
		head = smp_load_acquire(&g_head);          /* head: current head */
		WRITE_ONCE(node->next_idx, head);          /* my_node→next = head */
	} while (cmpxchg(&g_head, head, my_idx) != head);

	/* g_head = my_idx; g_pool[my_idx] →{slab} back on freelist */
}

static int __init ipool_init(void)
{
	struct pool_node *a, *b, *c;

	pool_init();

	a = pool_alloc();   /* a →{slab} */
	b = pool_alloc();   /* b →{slab} */

	if (a) { a->data = 42; pr_info("pool: alloc a idx=%lu\n", (unsigned long)(a - g_pool)); }
	if (b) { b->data = 99; pr_info("pool: alloc b idx=%lu\n", (unsigned long)(b - g_pool)); }

	/* Free b back to pool. */
	if (b) pool_free(b);   /* b returned to g_pool */

	/* Reallocate: should get b's slot back. */
	c = pool_alloc();   /* c →{slab} (likely b's old slot) */
	if (c)
		pr_info("pool: realloc c idx=%lu data=%d\n",
			(unsigned long)(c - g_pool), c->data);

	if (a) pool_free(a);
	if (c) pool_free(c);
	return 0;
}

static void __exit ipool_exit(void)
{
	pr_info("pool: head=%u\n", smp_load_acquire(&g_head));
}

module_init(ipool_init);
module_exit(ipool_exit);
