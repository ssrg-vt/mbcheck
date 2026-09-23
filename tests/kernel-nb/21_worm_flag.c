// SPDX-License-Identifier: GPL-2.0
// 21_worm_flag.c — Write-once-read-many flag (readiness indicator)
//
// Non-blocking pattern: WORM FLAG (one-time readiness signal).
//
// A boolean flag is set exactly once (by the initialiser) and thereafter only
// read.  This is the simplest possible non-blocking synchronisation: once the
// flag is set, all subsequent reads return true without any further ordering
// cost.
//
// The pattern is used extensively for:
//   - Readiness indicators (driver is ready, firmware loaded, etc.)
//   - Feature flags set at boot time
//   - "module is dying" flag in module exit paths
//
// Sequence:
//   Writer: set up work, then WRITE_ONCE(g_ready, true)
//   Reader: if (READ_ONCE(g_ready)) ... use the result
//
// The WRITE_ONCE/READ_ONCE pair prevents the compiler from caching the flag
// in a register across the check (KCSAN data-race avoidance).  On TSO
// architectures (x86) no hardware barrier is needed once the flag is set.
// On weakly-ordered architectures, the smp_load_acquire / smp_store_release
// pair in the associated pointer publish (see 20_worm_pointer) provides the
// full ordering guarantee.
//
// Key APIs (kernel-memory-ordering-apis.md §2.1):
//   WRITE_ONCE(x, val)   — volatile store (compiler barrier)
//   READ_ONCE(x)         — volatile load (compiler barrier)
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → work           HeapObjVar
//          g_work = work               g_work →{heap}
//          WRITE_ONCE(g_ready, true)   flag set
//   exit:  READ_ONCE(g_ready)          flag check
//          g_work → w                  w →{heap} (guarded by flag)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-21: WORM flag via WRITE_ONCE/READ_ONCE for readiness");

struct work_result {
	int  code;
	long output;
};

static bool               g_ready;
static struct work_result *g_work;

static int __init worm_flag_init(void)
{
	struct work_result *work;

	/* Do the work first, then announce readiness. */
	work = kmalloc(sizeof(*work), GFP_KERNEL);  /* HeapObjVar */
	if (!work)
		return -ENOMEM;

	work->code   = 0;
	work->output = 0xfeedcafe;

	/* Store the result pointer (plain store — single writer, no racing). */
	g_work = work;                              /* g_work →{heap} */

	/*
	 * Store-store barrier ensures g_work is visible before g_ready.
	 * Readers who observe g_ready==true are guaranteed to see g_work.
	 */
	smp_wmb();

	/*
	 * Announce: volatile store prevents compiler from hoisting this
	 * above the g_work store and the smp_wmb().
	 */
	WRITE_ONCE(g_ready, true);

	pr_info("worm_flag: work published (output=0x%lx)\n", work->output);
	return 0;
}

static void __exit worm_flag_exit(void)
{
	struct work_result *w;

	/* Volatile load: prevents caching in a register by the compiler. */
	if (!READ_ONCE(g_ready)) {
		pr_info("worm_flag: not yet ready\n");
		return;
	}

	/*
	 * Load-load barrier: g_ready load above must complete before
	 * reading g_work below.
	 */
	smp_rmb();

	w = g_work;                                 /* w →{heap} */
	if (w) {
		pr_info("worm_flag: code=%d output=0x%lx\n", w->code, w->output);
		kfree(w);
	}
}

module_init(worm_flag_init);
module_exit(worm_flag_exit);
