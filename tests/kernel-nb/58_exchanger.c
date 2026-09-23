// SPDX-License-Identifier: GPL-2.0
// 58_exchanger.c — Lock-free exchanger (symmetric two-thread slot swap)
//
// Non-blocking pattern: LOCK-FREE EXCHANGER (Scherer, Lea & Scott 2006).
//
// An exchanger allows exactly two threads to swap values: each thread deposits
// its own value into a shared slot and picks up the partner's value.  This
// is a symmetric rendezvous with data exchange — neither thread is a producer
// or consumer; both are simultaneously both.
//
// Protocol (Scherer–Lea exchanger):
//   EMPTY   → thread A CAS(slot, EMPTY, (WAITING, A_val))  → slot in WAITING state
//   WAITING → thread B reads A_val, CAS(slot, WAITING, (BUSY, B_val)) → paired!
//             thread A spins until state == BUSY, picks up B_val
//             thread A CAS(slot, BUSY, EMPTY) → reset
//
// Used in:
//   - Java's SynchronousQueue (Exchanger internally)
//   - folly::LifoSem with pairing optimisation
//   - Linux futex exchange (proposed)
//
// Ordering edges of interest:
//   thread_a: kmalloc() → obj_a →{heap}
//             CAS(g_slot, EMPTY, obj_a) → g_slot →{heap} (WAITING state)
//             spin load_acquire(g_slot_state) == BUSY
//             READ_ONCE(g_slot_val) →{heap} (obj_b, partner's value)
//   thread_b: load g_slot → obj_a →{heap} (A's value)
//             kmalloc() → obj_b →{heap}
//             CAS(g_slot, WAITING, obj_b) → g_slot →{heap} (BUSY state)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-58: lock-free exchanger — symmetric two-thread value swap");

enum xchg_state { XS_EMPTY = 0, XS_WAITING = 1, XS_BUSY = 2 };

struct xchg_item {
	int  owner;
	int  value;
};

/* The shared exchange slot: state and current value pointer. */
static atomic_t            g_slot_state;
static struct xchg_item   *g_slot_val;

/* Thread A: deposits its value and waits for partner. */
static struct xchg_item *exchanger_offer(struct xchg_item *my_item)
{
	struct xchg_item *partner_item;

	/* Try to move slot from EMPTY to WAITING with our value. */
	WRITE_ONCE(g_slot_val, my_item);                       /* g_slot_val →{heap} */
	if (atomic_cmpxchg(&g_slot_state, XS_EMPTY, XS_WAITING)
			!= XS_EMPTY)
		return NULL;  /* slot not empty; retry elsewhere */

	/* Spin until partner completes the exchange (BUSY). */
	while (atomic_read_acquire(&g_slot_state) != XS_BUSY)  /* ACQUIRE */
		cpu_relax();

	partner_item = READ_ONCE(g_slot_val);                   /* partner_item →{heap} */

	/* Reset slot to EMPTY for next exchange. */
	atomic_set_release(&g_slot_state, XS_EMPTY);

	return partner_item;   /* caller gets partner's value */
}

/* Thread B: sees a WAITING slot and completes the exchange. */
static struct xchg_item *exchanger_match(struct xchg_item *my_item)
{
	struct xchg_item *partner_item;

	if (atomic_read(&g_slot_state) != XS_WAITING)
		return NULL;  /* nothing to match */

	partner_item = READ_ONCE(g_slot_val);                   /* partner_item →{heap} */

	/* Deposit our value and signal the offerer (BUSY). */
	WRITE_ONCE(g_slot_val, my_item);                        /* g_slot_val →{heap} */
	if (atomic_cmpxchg(&g_slot_state, XS_WAITING, XS_BUSY)
			!= XS_WAITING)
		return NULL;  /* race: offerer timed out */

	return partner_item;   /* caller gets partner's value */
}

static int __init ex58_init(void)
{
	struct xchg_item *ia, *ib;
	struct xchg_item *got_a, *got_b;

	atomic_set(&g_slot_state, XS_EMPTY);

	ia = kmalloc(sizeof(*ia), GFP_KERNEL);   /* HeapObjVar (A's item) */
	ib = kmalloc(sizeof(*ib), GFP_KERNEL);   /* HeapObjVar (B's item) */
	if (!ia || !ib) {
		kfree(ia); kfree(ib);
		return -ENOMEM;
	}

	ia->owner = 0; ia->value = 111;
	ib->owner = 1; ib->value = 222;

	/* Thread A offers its item. */
	got_a = exchanger_offer(ia);    /* A gets B's item */

	/* Thread B matches. */
	got_b = exchanger_match(ib);    /* B gets A's item */

	/* In a real system got_a == ib and got_b == ia. */
	pr_info("ex58: A got value=%d, B got value=%d\n",
		got_a ? got_a->value : -1,
		got_b ? got_b->value : -1);

	return 0;
}

static void __exit ex58_exit(void)
{
	/* The items are still alive (freed here). */
	struct xchg_item *cur = READ_ONCE(g_slot_val);   /* cur →{heap|NULL} */

	/* Clean up: in the sequential simulation items may still be in slot. */
	kfree(cur);
	WRITE_ONCE(g_slot_val, NULL);
}

module_init(ex58_init);
module_exit(ex58_exit);
