// SPDX-License-Identifier: GPL-2.0
// 28_eventcount.c — EventCount (futex-like wake/wait without kernel involvement)
//
// Non-blocking pattern: EVENT COUNT (Dmitry Vyukov).
//
// An EventCount is a low-level primitive that bridges lock-free producers and
// blocking consumers.  The producer increments an atomic counter (the "event")
// using a fetch-add; the consumer samples the counter before checking the
// condition ("prepare_wait"), and if the condition is still false, it parks.
// Crucially, the counter read happens BEFORE the condition check, so a wake
// that fires between the condition check and the park is never lost.
//
// Protocol:
//   Consumer:
//     1. key = eventcount_prepare_wait()   — sample counter (acquire)
//     2. if (condition_true()) break;       — fast path: no wait needed
//     3. eventcount_wait(key)              — park if counter unchanged
//
//   Producer:
//     1. do work / update shared state
//     2. eventcount_notify()              — fetch_add(1) + wake waiters
//
// In the kernel this maps to:
//   prepare_wait: atomic_read_acquire(&g_ec)   — sample (acquire)
//   notify:       atomic_fetch_add(1, &g_ec)   — full barrier + wakeup
//   wait:         compare stored key with current; if equal, yield
//
// folly::EventCount is a production-quality implementation of this pattern.
//
// Ordering edges of interest:
//   notify:  atomic_fetch_add → g_data stores ordered before
//   prepare: atomic_read_acquire → g_data loads ordered after

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-28: EventCount pattern (futex-like producer/consumer sync)");

struct work_item {
	int seq;
	int data;
};

static atomic_t           g_ec   = ATOMIC_INIT(0);  /* event counter */
static struct work_item  *g_item;                    /* shared payload */

/* Notify: update payload, then bump event counter to wake waiters. */
static void ec_notify(struct work_item *item)
{
	/*
	 * Store the work item BEFORE incrementing the event counter.
	 * smp_store_release ensures item write is visible to any consumer
	 * that observes ec > old_key.
	 */
	smp_store_release(&g_item, item);          /* g_item →{heap} */

	/*
	 * Full barrier: payload store ordered before counter increment.
	 * atomic_fetch_add has full barrier semantics.
	 */
	atomic_fetch_add(1, &g_ec);                /* NOTIFY */
}

/* Prepare-wait: returns current counter value (key for wait). */
static int ec_prepare_wait(void)
{
	/*
	 * Acquire load: the counter read must complete before any
	 * subsequent condition check (g_item read).
	 */
	return atomic_read_acquire(&g_ec);         /* acquire */
}

/* Wait: spin until counter advances past key (simplified; real impl sleeps). */
static void ec_wait(int key)
{
	while (atomic_read_acquire(&g_ec) == key)
		cpu_relax();
}

static int __init ec_init(void)
{
	struct work_item *item;
	int key;

	/* Consumer samples the counter before checking condition. */
	key = ec_prepare_wait();                   /* key = current counter */

	/* Simulate producer side: item not yet available → consumer will wait. */
	item = kmalloc(sizeof(*item), GFP_KERNEL); /* HeapObjVar */
	if (!item)
		return -ENOMEM;
	item->seq  = 1;
	item->data = 0xec;

	ec_notify(item);                           /* publish + wake */

	pr_info("ec: notified with seq=%d key_was=%d\n", item->seq, key);
	return 0;
}

static void __exit ec_exit(void)
{
	struct work_item *p;

	/*
	 * In a real scenario consumer would call ec_wait(key) here.
	 * We just read the result directly (notification already fired).
	 */
	p = smp_load_acquire(&g_item);             /* p →{heap|NULL} */
	if (p) {
		pr_info("ec: consumed seq=%d data=0x%x\n", p->seq, p->data);
		kfree(p);
	}
}

module_init(ec_init);
module_exit(ec_exit);
