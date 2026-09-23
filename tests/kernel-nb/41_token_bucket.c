// SPDX-License-Identifier: GPL-2.0
// 41_token_bucket.c — Lock-free token bucket rate limiter
//
// Non-blocking pattern: ATOMIC TOKEN BUCKET (rate limiter).
//
// A token bucket rate limiter tracks available "tokens" in an atomic counter.
// Producers (refill) add tokens (up to a capacity); consumers subtract them.
// Refill is time-based: tokens are added proportional to elapsed ns.
//
// Non-blocking: both refill and consume use atomic RMW (fetch_add, cmpxchg)
// and never block.  If consume fails (insufficient tokens) it returns false
// without waiting.
//
// This pattern appears in:
//   - folly::TokenBucket
//   - Linux tc (traffic control) token bucket filter
//   - Network rate limiters in the kernel (net/sched/sch_tbf.c style)
//   - eBPF rate-limiting programs
//
// Two variants shown:
//   1. Integer token count (simple, coarse): atomic_long_t
//   2. Saturating add (capacity clamping): cmpxchg loop
//
// Key APIs: atomic_long_fetch_add (§1.3), atomic_long_cmpxchg (§1.3),
//           ktime_get (non-atomic, for timestamps)
//
// Ordering edges of interest:
//   refill: ktime snapshot; atomic_long_cmpxchg on g_tokens (token refill)
//           g_last_refill_ns = now (WRITE_ONCE, monotone)
//   consume: atomic_long_read_acquire → tokens; cmpxchg to subtract

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/ktime.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-41: lock-free token bucket rate limiter");

#define TB_RATE_PER_NS   1          /* tokens per nanosecond  */
#define TB_CAPACITY      1000       /* maximum token count    */

static atomic_long_t g_tokens        = ATOMIC_LONG_INIT(0);
static u64           g_last_refill_ns;  /* last refill timestamp (ns) */

/* Refill g_tokens based on elapsed time since last refill.
 * Uses a CAS loop to avoid over-refill. */
static void tb_refill(void)
{
	u64 now, last, elapsed;
	long earned, old_val, new_val;

	now  = ktime_get_ns();
	last = READ_ONCE(g_last_refill_ns);       /* last refill timestamp */

	if (now <= last)
		return;

	elapsed = now - last;
	earned  = (long)(elapsed * TB_RATE_PER_NS);

	/* Clamp earned to remaining capacity. */
	do {
		old_val = atomic_long_read(&g_tokens);     /* old_val: current tokens */
		new_val = old_val + earned;
		if (new_val > TB_CAPACITY)
			new_val = TB_CAPACITY;
		if (new_val == old_val)
			break;  /* already at capacity */
	} while (atomic_long_cmpxchg(&g_tokens, old_val, new_val) != old_val);

	WRITE_ONCE(g_last_refill_ns, now);         /* advance timestamp */
}

/* Try to consume `cost` tokens.  Returns true if granted. */
static bool tb_consume(long cost)
{
	long old_val, new_val;

	do {
		old_val = atomic_long_read_acquire(&g_tokens);
		if (old_val < cost)
			return false;   /* not enough tokens — non-blocking reject */
		new_val = old_val - cost;
	} while (atomic_long_cmpxchg(&g_tokens, old_val, new_val) != old_val);

	return true;   /* tokens consumed */
}

static int __init tb_init(void)
{
	/* Seed with half-capacity. */
	atomic_long_set(&g_tokens, TB_CAPACITY / 2);
	g_last_refill_ns = ktime_get_ns();

	tb_refill();

	if (tb_consume(100))
		pr_info("tb: consumed 100 tokens OK\n");
	else
		pr_info("tb: consume 100 FAILED\n");

	if (tb_consume(TB_CAPACITY))
		pr_info("tb: consumed full capacity (unexpected)\n");
	else
		pr_info("tb: correctly rejected over-capacity consume\n");

	return 0;
}

static void __exit tb_exit(void)
{
	pr_info("tb: remaining tokens=%ld\n", atomic_long_read(&g_tokens));
}

module_init(tb_init);
module_exit(tb_exit);
