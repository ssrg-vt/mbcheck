// SPDX-License-Identifier: GPL-2.0
// 07_event_store_poll.c — One-shot event via WRITE_ONCE flag + READ_ONCE poll
//
// Non-blocking pattern: store-poll event using plain volatile accesses.
//
// The simplest possible non-blocking event: a single integer flag written
// exactly once by the producer and polled by the consumer.  The key
// ordering guarantee comes from the explicit smp_wmb() / smp_rmb() barriers
// around WRITE_ONCE / READ_ONCE, making it SVF-friendly (no asm coercion).
//
// Sequence:
//   Producer:
//     1. Prepare heap payload
//     2. WRITE_ONCE(g_payload, ptr)   — volatile store of the pointer
//     3. smp_wmb()                    — store-store barrier
//     4. WRITE_ONCE(g_flag, 1)        — announce readiness
//
//   Consumer:
//     1. Poll READ_ONCE(g_flag) until 1
//     2. smp_rmb()                    — load-load barrier
//     3. p = READ_ONCE(g_payload)     — safe to read now
//
// Folly analogue: folly::SaturatingSemaphore (spin-only variant).
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → p              HeapObjVar
//          WRITE_ONCE(g_payload, p)   g_payload →{heap}  (volatile store)
//   exit:  READ_ONCE(g_payload) → q   q →{heap}          (volatile load)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-07: one-shot event via WRITE_ONCE flag + READ_ONCE poll");

struct event_data {
	int  id;
	long value;
};

static int               g_flag;
static struct event_data *g_payload;

static int __init poll07_init(void)
{
	struct event_data *p;

	/* [Producer] ------------------------------------------------------- */
	p = kmalloc(sizeof(*p), GFP_KERNEL);      /* HeapObjVar */
	if (!p)
		return -ENOMEM;

	p->id    = 7;
	p->value = 0xc0ffee;

	/*
	 * Volatile store of the pointer: SVF sees a direct `store volatile ptr`
	 * — no integer coercion, no inline asm.
	 */
	WRITE_ONCE(g_payload, p);                 /* g_payload →{heap} */

	/*
	 * Store-store barrier: g_payload write must be visible before
	 * g_flag write.
	 */
	smp_wmb();

	WRITE_ONCE(g_flag, 1);                    /* announce */
	return 0;
}

static void __exit poll07_exit(void)
{
	struct event_data *q;

	/* [Consumer] — spin until flag is set ------------------------------ */
	while (!READ_ONCE(g_flag))
		cpu_relax();

	/*
	 * Load-load barrier: the g_flag load above must complete before
	 * the g_payload load below.
	 */
	smp_rmb();

	q = READ_ONCE(g_payload);                 /* q →{heap} */
	if (q) {
		pr_info("poll07: id=%d value=0x%lx\n", q->id, q->value);
		kfree(q);
	}
}

module_init(poll07_init);
module_exit(poll07_exit);
