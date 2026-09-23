// SPDX-License-Identifier: GPL-2.0
// 48_mpmc_ring.c — Lock-free MPMC ring buffer (Dmitry Vyukov / folly MPMCQueue)
//
// Non-blocking pattern: MPMC BOUNDED RING BUFFER.
//
// Unlike the SPSC ring (09) which uses a single producer and consumer, this
// ring allows *multiple* producers and *multiple* consumers to operate
// concurrently.  Each slot carries an atomic sequence number (a "lap" tag)
// that determines ownership:
//
//   sequence == pos        → slot is empty; producer may claim it
//   sequence == pos + 1    → slot is full; consumer may claim it
//
// This is Vyukov's bounded MPMC queue (used in folly::MPMCQueue and the
// Linux io_uring submission/completion rings).
//
// Producers:
//   1. FAA(g_enqueue_pos) → my position.
//   2. Spin until slot[pos % CAP].sequence == pos.
//   3. Write data to slot, store_release(sequence, pos + 1).
//
// Consumers:
//   1. FAA(g_dequeue_pos) → my position.
//   2. Spin until slot[pos % CAP].sequence == pos + 1.
//   3. Read data from slot, store_release(sequence, pos + CAP).
//
// Ordering edges of interest:
//   enqueue: kmalloc() → item →{heap}; store_release(slot->sequence, pos+1)
//   dequeue: load_acquire(slot->sequence) == pos+1; item →{heap} (via slot->ptr)
//            store_release(slot->sequence, pos + CAP)
//
// Key APIs: atomic_long_fetch_add (§1.3), smp_store_release (§2.2),
//           smp_load_acquire (§2.2), WRITE_ONCE / READ_ONCE.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-48: lock-free MPMC bounded ring buffer (Vyukov-style)");

#define RING_CAP  8   /* must be power of two */

struct ring_item {
	int  id;
	int  payload;
};

struct ring_slot {
	atomic_long_t  sequence;    /* lap tag */
	struct ring_item *ptr;      /* pointer to heap-allocated item */
};

/* The ring storage. */
static struct ring_slot g_ring[RING_CAP];

/* Shared producer / consumer position counters. */
static atomic_long_t g_enqueue_pos = ATOMIC_LONG_INIT(0);
static atomic_long_t g_dequeue_pos = ATOMIC_LONG_INIT(0);

static void ring48_init_slots(void)
{
	int i;

	for (i = 0; i < RING_CAP; i++)
		atomic_long_set(&g_ring[i].sequence, i);  /* initial lap tag */
}

/* Enqueue one item.  Returns true on success, false if full (spin version
 * simplified to a single attempt for the litmus). */
static bool ring48_enqueue(struct ring_item *item)
{
	long pos, seq, diff;
	struct ring_slot *slot;

	pos = atomic_long_fetch_add(1, &g_enqueue_pos);  /* claim position */

	slot = &g_ring[pos % RING_CAP];

	/* Spin until the slot is ready for us (sequence == pos). */
	for (;;) {
		seq  = atomic_long_read_acquire(&slot->sequence);   /* ACQUIRE */
		diff = seq - pos;
		if (diff == 0)
			break;
		if (diff < 0)
			return false;  /* queue full (simplified: single attempt) */
		cpu_relax();
	}

	WRITE_ONCE(slot->ptr, item);                              /* item →{heap} */
	smp_store_release(&slot->sequence.counter, pos + 1);     /* PUBLISH */
	return true;
}

/* Dequeue one item.  Returns pointer or NULL if empty. */
static struct ring_item *ring48_dequeue(void)
{
	long pos, seq, diff;
	struct ring_slot *slot;
	struct ring_item *item;

	pos = atomic_long_fetch_add(1, &g_dequeue_pos);  /* claim position */

	slot = &g_ring[pos % RING_CAP];

	/* Spin until the slot has data (sequence == pos + 1). */
	for (;;) {
		seq  = atomic_long_read_acquire(&slot->sequence);   /* ACQUIRE */
		diff = seq - (pos + 1);
		if (diff == 0)
			break;
		if (diff < 0)
			return NULL;  /* empty */
		cpu_relax();
	}

	item = READ_ONCE(slot->ptr);                              /* CONSUME: item →{heap} */
	smp_store_release(&slot->sequence.counter, pos + RING_CAP);  /* recycle slot */
	return item;
}

static int __init ring48_init(void)
{
	struct ring_item *a, *b, *c;
	struct ring_item *out;

	ring48_init_slots();

	/* Producer 1 */
	a = kmalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->id = 1; a->payload = 100;
	ring48_enqueue(a);

	/* Producer 2 */
	b = kmalloc(sizeof(*b), GFP_KERNEL);
	if (!b) { kfree(a); return -ENOMEM; }
	b->id = 2; b->payload = 200;
	ring48_enqueue(b);

	/* Producer 3 */
	c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (!c) { kfree(a); kfree(b); return -ENOMEM; }
	c->id = 3; c->payload = 300;
	ring48_enqueue(c);

	pr_info("ring48: enqueued 3 items\n");
	return 0;
}

static void __exit ring48_exit(void)
{
	struct ring_item *out;

	/* Consumer 1 and 2 drain the ring. */
	while ((out = ring48_dequeue()) != NULL) {   /* out →{heap} */
		pr_info("ring48: dequeued id=%d payload=%d\n", out->id, out->payload);
		kfree(out);
	}
}

module_init(ring48_init);
module_exit(ring48_exit);
