// SPDX-License-Identifier: GPL-2.0
// 03_mp_rcu.c — Message-Passing via RCU (rcu_assign_pointer / rcu_dereference)
//
// Non-blocking pattern: RCU pointer publish.
//
// RCU (Read-Copy-Update) is the kernel's canonical "read-mostly" non-blocking
// pattern.  Readers are never blocked (no lock acquired); they observe a
// consistent snapshot by staying within an RCU read-side critical section.
// Writers perform a copy-and-swap: allocate a new version, initialise it,
// publish via rcu_assign_pointer (which wraps smp_store_release), then call
// synchronize_rcu() to wait for all pre-existing read-side critical sections
// to finish before freeing the old object.
//
// Key APIs (from kernel-memory-ordering-apis.md §3.4):
//   rcu_assign_pointer(p, v)    — store-release (wraps smp_store_release)
//   rcu_dereference(p)          — load-acquire (wraps READ_ONCE)
//   rcu_read_lock/unlock()      — delimit read-side critical section
//   synchronize_rcu()           — full barrier on all CPUs after all readers
//
// Folly analogue: folly::rcu_domain with retire() and rcu_reader.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → new_cfg             HeapObjVar (new)
//          rcu_assign_pointer(g_cfg, new)  g_cfg →{heap}  (release store)
//   exit:  rcu_dereference(g_cfg) → p      p →{heap} (acquire load)
//          synchronize_rcu()              full barrier before kfree

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-03: message passing via rcu_assign_pointer / rcu_dereference");

struct config {
	int  version;
	int  param;
};

/* RCU-protected global pointer. */
static struct config __rcu *g_cfg;

static int __init mp03_init(void)
{
	struct config *new_cfg;

	/* [Writer] --------------------------------------------------------- */
	new_cfg = kmalloc(sizeof(*new_cfg), GFP_KERNEL);  /* HeapObjVar */
	if (!new_cfg)
		return -ENOMEM;

	new_cfg->version = 1;
	new_cfg->param   = 42;

	/*
	 * Publish: release store so readers see a fully-initialised object.
	 * Internally: smp_store_release(&g_cfg, new_cfg).
	 */
	rcu_assign_pointer(g_cfg, new_cfg);               /* g_cfg →{heap} */
	return 0;
}

static void __exit mp03_exit(void)
{
	struct config *old_cfg;

	/* [Reader] --------------------------------------------------------- */
	rcu_read_lock();

	/*
	 * Acquire load inside RCU read-side critical section.
	 * Internally: READ_ONCE(g_cfg) with an acquire barrier.
	 */
	old_cfg = rcu_dereference(g_cfg);                 /* old_cfg →{heap} */
	if (old_cfg)
		pr_info("mp03: version=%d param=%d\n",
			old_cfg->version, old_cfg->param);

	rcu_read_unlock();

	/*
	 * [Writer cleanup] Wait for all readers that may have seen the old
	 * pointer to finish, then free safely.
	 */
	synchronize_rcu();
	kfree(old_cfg);
}

module_init(mp03_init);
module_exit(mp03_exit);
