// SPDX-License-Identifier: GPL-2.0
// 02_mp_write_once_mb.c — Message-Passing via WRITE_ONCE + smp_wmb / READ_ONCE + smp_rmb
//
// Non-blocking pattern: MP with explicit store/load barriers.
//
// Instead of the combined release/acquire macros, this pattern uses
// separate store-store and load-load barriers around plain volatile
// accesses (WRITE_ONCE / READ_ONCE).  The sequence is:
//
//   Producer:
//     1. Write payload data (ordinary stores)
//     2. smp_wmb()           — store-store barrier: all data stores above
//                              complete before the pointer store below
//     3. WRITE_ONCE(g_ptr, msg)  — pointer publish (volatile store)
//
//   Consumer:
//     1. p = READ_ONCE(g_ptr)    — volatile load of the pointer
//     2. smp_rmb()           — load-load barrier: pointer load above
//                              completes before any payload reads below
//     3. Read payload data via p
//
// The WRITE_ONCE/READ_ONCE version is useful when the analysis tool
// cannot track through smp_store_release's inline-asm constraint coercion:
// WRITE_ONCE emits a plain `store volatile ptr` that is directly visible
// to SVF's pointer analysis.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → msg               HeapObjVar
//          smp_wmb()                      store-store fence
//          WRITE_ONCE(g_ptr, msg)         g_ptr →{heap} (volatile store)
//   exit:  READ_ONCE(g_ptr) → p           p →{heap} (volatile load)
//          smp_rmb()                      load-load fence
//          p->value                       field read through acquired ptr

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-02: message passing via WRITE_ONCE+smp_wmb / READ_ONCE+smp_rmb");

struct msg_data {
	int   seq;
	int   value;
};

static struct msg_data *g_ptr;

static int __init mp02_init(void)
{
	struct msg_data *msg;

	/* [Producer] ------------------------------------------------------- */
	msg = kmalloc(sizeof(*msg), GFP_KERNEL);   /* HeapObjVar */
	if (!msg)
		return -ENOMEM;

	/* Ordinary stores to payload fields. */
	msg->seq   = 1;
	msg->value = 100;

	/*
	 * Store-store barrier: the payload writes above must be visible to
	 * other CPUs before the pointer store below.
	 */
	smp_wmb();

	/*
	 * Volatile store: emits a plain `store volatile ptr` — pointer is
	 * directly visible to pointer analysis without asm coercion.
	 */
	WRITE_ONCE(g_ptr, msg);                    /* PUBLISH: g_ptr →{heap} */
	return 0;
}

static void __exit mp02_exit(void)
{
	struct msg_data *p;

	/* [Consumer] ------------------------------------------------------- */

	/*
	 * Volatile load: emits a plain `load volatile ptr` — SVF can follow
	 * the flow g_ptr → p → {heap}.
	 */
	p = READ_ONCE(g_ptr);                      /* CONSUME: p →{heap} */
	if (!p)
		return;

	/*
	 * Load-load barrier: the pointer load above must complete before we
	 * read the payload fields below.
	 */
	smp_rmb();

	pr_info("mp02: seq=%d value=%d\n", p->seq, p->value);
	kfree(p);
}

module_init(mp02_init);
module_exit(mp02_exit);
