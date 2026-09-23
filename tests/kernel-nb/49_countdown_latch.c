// SPDX-License-Identifier: GPL-2.0
// 49_countdown_latch.c — Lock-free countdown latch (N-way join synchronisation)
//
// Non-blocking pattern: COUNTDOWN LATCH (N-arrivals → release waiters).
//
// A countdown latch starts at N.  Each "arrival" atomically decrements the
// counter.  When the counter reaches zero, all waiting threads may proceed.
// The pattern is *not* producer–consumer: all N participants are peers — any
// of them can be the last to arrive and thereby release the waiters.
//
// This is the kernel analogue of:
//   - Java CountDownLatch
//   - folly::Baton (N=1)
//   - Linux kernel's completion (generalised)
//   - C++20 std::latch
//
// Protocol:
//   arrive():   atomic_dec_and_test(&g_count) → if true: store_release(g_open, 1)
//   wait():     spin on load_acquire(g_open) until non-zero
//   work():     read g_result →{heap} after latch opens
//
// This litmus uses 3 simulated "arrivals" in module_init and one waiter in
// module_exit.  The arrivals are *not* producer→consumer; every arrival is a
// peer that contributes work (stored into g_results[]).
//
// Ordering edges of interest:
//   arrive[0]: kmalloc() → g_results[0] →{heap}; dec_and_test(g_count)
//   arrive[1]: kmalloc() → g_results[1] →{heap}; dec_and_test(g_count)
//   arrive[2]: kmalloc() → g_results[2] →{heap}; dec_and_test(g_count) → 0
//              smp_store_release(&g_open, 1)   ← latch opens
//   wait:      smp_load_acquire(&g_open) → 1
//              g_results[i] →{heap} (all three visible)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-49: countdown latch — N-way peer join synchronisation");

#define LATCH_N  3

struct work_result {
	int  contributor;
	int  value;
};

/* Shared results array: each arrival writes one slot. */
static struct work_result *g_results[LATCH_N];

/* Latch state. */
static atomic_t g_count = ATOMIC_INIT(LATCH_N);   /* counts down to 0 */
static atomic_t g_open  = ATOMIC_INIT(0);          /* 0=closed, 1=open */

/* Each peer calls arrive() after completing its contribution. */
static void latch49_arrive(int id, int value)
{
	struct work_result *r;

	r = kmalloc(sizeof(*r), GFP_KERNEL);   /* HeapObjVar */
	if (!r)
		return;
	r->contributor = id;
	r->value       = value;

	/* Publish result before decrementing. */
	smp_store_release(&g_results[id], r);          /* g_results[id] →{heap} */

	/* Decrement; last arrival opens the latch. */
	if (atomic_dec_and_test(&g_count))
		atomic_set_release(&g_open, 1);            /* OPEN: release */
}

/* Waiter spins until all arrivals are in. */
static void latch49_wait(void)
{
	while (!atomic_read_acquire(&g_open))          /* ACQUIRE */
		cpu_relax();
}

static int __init latch49_init(void)
{
	/* Three peer arrivals (simulated sequentially; in reality these are
	 * concurrent threads, but a single-TU litmus must serialize them). */
	latch49_arrive(0, 10);
	latch49_arrive(1, 20);
	latch49_arrive(2, 30);   /* last arrival: opens the latch */
	pr_info("latch49: all peers arrived, latch open\n");
	return 0;
}

static void __exit latch49_exit(void)
{
	int i;

	latch49_wait();   /* blocks until latch open (already open in this litmus) */

	/* All g_results[] are visible because of the acquire above. */
	for (i = 0; i < LATCH_N; i++) {
		struct work_result *r = smp_load_acquire(&g_results[i]);  /* r →{heap} */
		if (r) {
			pr_info("latch49: result[%d] contributor=%d value=%d\n",
				i, r->contributor, r->value);
			kfree(r);
		}
	}
}

module_init(latch49_init);
module_exit(latch49_exit);
