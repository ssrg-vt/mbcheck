// SPDX-License-Identifier: GPL-2.0
// 56_rendezvous.c — Two-thread rendezvous barrier (symmetric peer synchronisation)
//
// Non-blocking pattern: RENDEZVOUS BARRIER (symmetric, two-peer).
//
// A rendezvous barrier requires *both* threads to arrive before either may
// proceed.  Unlike a countdown latch (49), which has a one-directional
// signaller→waiter structure, a rendezvous is fully symmetric: each peer
// publishes its arrival and then waits for the other.
//
// This is the kernel analogue of:
//   - POSIX pthread_barrier_t (for N=2)
//   - C++20 std::barrier (N=2)
//   - Java's CyclicBarrier (N=2)
//   - Linux futex-based barrier
//
// Protocol (symmetric, no producer/consumer roles):
//   Thread A:  store_release(&g_arrived[A], 1)
//              spin load_acquire(&g_arrived[B]) until 1
//              proceed (read g_result_b →{heap})
//   Thread B:  store_release(&g_arrived[B], 1)
//              spin load_acquire(&g_arrived[A]) until 1
//              proceed (read g_result_a →{heap})
//
// Each peer publishes its own result *before* signalling arrival, so the
// other peer is guaranteed to see it after the rendezvous.
//
// Ordering edges of interest:
//   peer_a: kmalloc() → g_result_a →{heap}
//           smp_store_release(&g_arrived[0], 1)   ← announces arrival
//           smp_load_acquire(&g_arrived[1]) → 1   ← waits for peer B
//           READ_ONCE(g_result_b) →{heap}          ← sees peer B's result
//   peer_b: kmalloc() → g_result_b →{heap}
//           smp_store_release(&g_arrived[1], 1)
//           smp_load_acquire(&g_arrived[0]) → 1
//           READ_ONCE(g_result_a) →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-56: two-peer rendezvous barrier (symmetric, no roles)");

struct peer_result {
	int  peer_id;
	int  value;
};

/* Per-peer results: each peer writes its own slot before signalling. */
static struct peer_result *g_result[2];

/* Arrival flags: 0 = not arrived, 1 = arrived. */
static atomic_t g_arrived[2];

/* Perform a rendezvous as peer 'id' (0 or 1), publishing value 'val'. */
static void rendezvous(int id, int val)
{
	struct peer_result *r;
	int other = 1 - id;

	/* Allocate and fill our result. */
	r = kmalloc(sizeof(*r), GFP_KERNEL);   /* HeapObjVar */
	if (!r)
		return;
	r->peer_id = id;
	r->value   = val;

	/* Publish result before announcing arrival. */
	smp_store_release(&g_result[id], r);           /* g_result[id] →{heap} */

	/* Announce our arrival. */
	atomic_set_release(&g_arrived[id], 1);         /* ARRIVE: release */

	/* Wait for the other peer. */
	while (!atomic_read_acquire(&g_arrived[other]))  /* ACQUIRE */
		cpu_relax();

	/* Now g_result[other] is guaranteed visible. */
}

static int __init rv56_init(void)
{
	atomic_set(&g_arrived[0], 0);
	atomic_set(&g_arrived[1], 0);

	/*
	 * Simulated: in a real system these two calls are in concurrent threads.
	 * In this single-TU litmus they run sequentially, which still exposes
	 * the ordering edges to the pointer analyser (both sides in one TU).
	 */
	rendezvous(0, 100);   /* Peer A arrives, waits for B. */
	rendezvous(1, 200);   /* Peer B arrives, waits for A (already there). */

	pr_info("rv56: both peers rendezvoused\n");
	return 0;
}

static void __exit rv56_exit(void)
{
	struct peer_result *ra, *rb;

	/* Both results visible after rendezvous acquire. */
	ra = smp_load_acquire(&g_result[0]);   /* ra →{heap} */
	rb = smp_load_acquire(&g_result[1]);   /* rb →{heap} */

	if (ra)
		pr_info("rv56: peer 0 value=%d\n", ra->value);
	if (rb)
		pr_info("rv56: peer 1 value=%d\n", rb->value);

	kfree(ra);
	kfree(rb);
}

module_init(rv56_init);
module_exit(rv56_exit);
