// SPDX-License-Identifier: GPL-2.0
// 23_asymmetric_barrier.c — Asymmetric barrier (cheap readers, expensive writers)
//
// Non-blocking pattern: ASYMMETRIC THREAD FENCE.
//
// An asymmetric barrier is cheap on one side (typically the fast/read path)
// and expensive on the other (the slow/write path).  The classic kernel
// implementation uses:
//   - Reader side:  a compiler barrier only (smp_mb__after_ctrl_dep or
//                   just barrier()), making the read path essentially free.
//   - Writer side:  a full IPI-based flush (synchronize_rcu() or
//                   kick_all_cpus_sync()) that acts as a store-load fence
//                   across all CPUs.
//
// This is used in:
//   - RCU grace periods (readers are cheap, synchronize_rcu is expensive)
//   - Memory-mapped notification (mprotect + signal flush)
//   - folly::AsymmetricThreadFence: membarrier() syscall on Linux
//     (MEMBARRIER_CMD_GLOBAL on writer, compiler fence on reader)
//
// Linux kernel equivalent: the membarrier() system call infrastructure, and
// smp_mb__after_ctrl_dep() + synchronize_rcu() used in kernel/rcu/ for the
// same asymmetry property.
//
// Pointer-analysis interest:
//   write side: kmalloc() → obj → published via WRITE_ONCE after full barrier
//   read side : READ_ONCE → pointer after compiler-only fence
//
// Key APIs:
//   barrier()          — compiler-only fence (§2.1)
//   synchronize_rcu()  — full cross-CPU barrier on writer side (§3.4)
//   WRITE_ONCE / READ_ONCE  — volatile accesses (§2.1)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-23: asymmetric barrier — cheap reader fence, expensive writer fence");

struct asym_data {
	int version;
	int payload;
};

static struct asym_data *g_data;

/* Writer side: full cross-CPU barrier (expensive). */
static void asym_write(int ver, int val)
{
	struct asym_data *old, *neo;

	neo = kmalloc(sizeof(*neo), GFP_KERNEL);   /* HeapObjVar */
	if (!neo)
		return;
	neo->version = ver;
	neo->payload = val;

	old = g_data;

	/* Store-store: all neo fields visible before pointer publish. */
	smp_wmb();
	WRITE_ONCE(g_data, neo);                   /* g_data →{heap} */

	/*
	 * Writer-side expensive fence: waits for all CPUs to observe the
	 * new g_data before we free the old object.  This is the "asymmetric"
	 * part: the reader pays only a compiler barrier.
	 */
	synchronize_rcu();                         /* full cross-CPU barrier */
	kfree(old);
}

/* Reader side: compiler-only fence (cheap). */
static struct asym_data *asym_read(void)
{
	struct asym_data *p;

	/*
	 * Compiler barrier only: prevents the compiler from reordering
	 * the READ_ONCE below with earlier control-flow decisions.
	 * On TSO (x86) this is sufficient for the reader; on weakly-ordered
	 * ISAs the writer's synchronize_rcu() provides the global ordering.
	 */
	barrier();
	p = READ_ONCE(g_data);                     /* p →{heap|NULL} */
	return p;
}

static int __init asym_init(void)
{
	asym_write(1, 0xabc);
	pr_info("asym: write done\n");
	return 0;
}

static void __exit asym_exit(void)
{
	struct asym_data *p = asym_read();         /* p →{heap} */

	if (p)
		pr_info("asym: version=%d payload=0x%x\n", p->version, p->payload);

	/* Final cleanup (no concurrent readers at module exit). */
	kfree(READ_ONCE(g_data));
}

module_init(asym_init);
module_exit(asym_exit);
