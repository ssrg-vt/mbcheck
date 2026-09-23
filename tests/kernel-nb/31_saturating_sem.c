// SPDX-License-Identifier: GPL-2.0
// 31_saturating_sem.c — Saturating semaphore (count clamped at maximum)
//
// Non-blocking pattern: SATURATING SEMAPHORE (folly::SaturatingSemaphore).
//
// A saturating semaphore is a binary (or N-valued) semaphore whose count
// clamps at a maximum and never overflows.  The key property for NB analysis:
//   post: CAS loop — increment count if < MAX, else clamp at MAX
//   wait: spin until count > 0, then CAS-decrement
//
// The simplest (binary) variant is a one-bit flag equivalent to a baton,
// but the saturating N-variant uses atomic_fetch_add with post-CAS fixup.
//
// This is used in kernel workqueue logic (nr_running clamped), rate limiters,
// and token-bucket counters.
//
// Key APIs:
//   atomic_cmpxchg (§1.1) — CAS for clamped increment and decrement
//   atomic_read_acquire   — acquire load for the spin
//
// Ordering edges of interest:
//   post: kmalloc → item →{heap}; WRITE_ONCE(g_item, item); atomic inc
//   wait: atomic_read_acquire > 0 → READ_ONCE(g_item) → item →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-31: saturating semaphore with clamped atomic counter");

#define SEM_MAX 4

struct sem_payload {
	int token;
	int value;
};

static atomic_t           g_sem  = ATOMIC_INIT(0);
static struct sem_payload *g_item;

/* Post: increment count up to SEM_MAX (saturating). */
static void satsem_post(void)
{
	int old, new;

	do {
		old = atomic_read(&g_sem);
		if (old >= SEM_MAX)
			return;  /* saturated: drop the post */
		new = old + 1;
	} while (atomic_cmpxchg(&g_sem, old, new) != old);
	/* atomic_cmpxchg has full barrier semantics */
}

/* Wait: spin until count > 0, then CAS-decrement (non-blocking spin). */
static void satsem_wait(void)
{
	int old;

	do {
		do {
			old = atomic_read_acquire(&g_sem);  /* acquire spin */
		} while (old <= 0);
	} while (atomic_cmpxchg(&g_sem, old, old - 1) != old);
}

static int __init satsem_init(void)
{
	struct sem_payload *item;

	item = kmalloc(sizeof(*item), GFP_KERNEL);  /* HeapObjVar */
	if (!item)
		return -ENOMEM;
	item->token = 42;
	item->value = 0xfeed;

	/* Publish the item before posting the semaphore. */
	smp_store_release(&g_item, item);           /* g_item →{heap} */

	satsem_post();
	satsem_post();
	satsem_post();
	/* Attempt to post beyond MAX — should saturate. */
	satsem_post();
	satsem_post();

	pr_info("satsem: count=%d (max=%d)\n", atomic_read(&g_sem), SEM_MAX);
	return 0;
}

static void __exit satsem_exit(void)
{
	struct sem_payload *p;

	satsem_wait();                              /* consume one token */

	p = smp_load_acquire(&g_item);              /* p →{heap|NULL} */
	if (p) {
		pr_info("satsem: token=%d value=0x%x\n", p->token, p->value);
		kfree(p);
	}
}

module_init(satsem_init);
module_exit(satsem_exit);
