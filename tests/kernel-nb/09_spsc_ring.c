// SPDX-License-Identifier: GPL-2.0
// 09_spsc_ring.c — Single-producer single-consumer ring buffer
//
// Non-blocking pattern: SPSC RING BUFFER (wait-free for both producer and consumer).
//
// The SPSC (Single-Producer Single-Consumer) ring buffer is wait-free for both
// sides: the producer never waits for the consumer and vice versa.  Correctness
// relies on:
//   - The producer performs a release store on the tail index after writing
//     the slot, so the consumer's acquire load of tail sees the filled slot.
//   - The consumer performs a release store on the head index after consuming
//     a slot, so the producer's acquire load of head sees the freed slot.
//
// Each slot holds a pointer to a heap-allocated object.
//
// Key APIs (kernel-memory-ordering-apis.md §2.2):
//   smp_store_release(&tail, new_tail) — tail update visible after data write
//   smp_load_acquire(&tail)            — tail read paired with above release
//
// Folly analogue: folly::ProducerConsumerQueue<T*>
//
// Ordering edges of interest for pointer analysis:
//   produce: kmalloc() → item           HeapObjVar
//            slots[tail] = item          slot →{heap}
//            smp_store_release(tail)     release: slot write ordered before
//   consume: smp_load_acquire(tail)      acquire: slot →{heap} visible
//            slots[head] → item          item →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-09: SPSC ring buffer with release/acquire on indices");

#define RING_SIZE 4   /* must be power of two */

struct ring_item {
	int seq;
	int value;
};

/* Ring buffer state. */
static struct ring_item *g_slots[RING_SIZE];
static unsigned int      g_head;   /* consumer index (read by producer) */
static unsigned int      g_tail;   /* producer index (read by consumer) */

static inline unsigned int ring_next(unsigned int idx)
{
	return (idx + 1) & (RING_SIZE - 1);
}

/* Try to enqueue item.  Returns false if full. */
static bool ring_push(struct ring_item *item)
{
	unsigned int tail = g_tail;
	unsigned int next = ring_next(tail);

	if (next == smp_load_acquire(&g_head))  /* full? */
		return false;

	g_slots[tail] = item;                   /* slot →{heap} */

	/* Release: slot write happens-before any consumer that reads this tail. */
	smp_store_release(&g_tail, next);
	return true;
}

/* Try to dequeue.  Returns NULL if empty. */
static struct ring_item *ring_pop(void)
{
	struct ring_item *item;
	unsigned int head = g_head;

	/* Acquire: pairs with smp_store_release(&g_tail) in ring_push. */
	if (head == smp_load_acquire(&g_tail))  /* empty? */
		return NULL;

	item = g_slots[head];                   /* item →{heap} */

	/* Release: tell producer this slot is free. */
	smp_store_release(&g_head, ring_next(head));
	return item;
}

static int __init spsc_init(void)
{
	int i;

	for (i = 0; i < RING_SIZE - 1; i++) {
		struct ring_item *item = kmalloc(sizeof(*item), GFP_KERNEL);

		if (!item)
			return -ENOMEM;
		item->seq   = i;
		item->value = i * 10;
		if (!ring_push(item)) {
			kfree(item);
			break;
		}
	}
	pr_info("spsc: enqueued items\n");
	return 0;
}

static void __exit spsc_exit(void)
{
	struct ring_item *item;

	while ((item = ring_pop()) != NULL) {   /* item →{heap} */
		pr_info("spsc: seq=%d value=%d\n", item->seq, item->value);
		kfree(item);
	}
}

module_init(spsc_init);
module_exit(spsc_exit);
