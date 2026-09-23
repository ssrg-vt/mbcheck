// SPDX-License-Identifier: GPL-2.0
// 11_cas_update.c — CAS-based pointer update loop
//
// Non-blocking pattern: LOCK-FREE POINTER UPDATE via cmpxchg loop.
//
// A global pointer g_current is updated to a new heap object atomically.
// The updater:
//   1. Reads the current value.
//   2. Allocates and initialises a new object.
//   3. CAS(g_current, observed_old, new_obj) — succeeds only if no other
//      updater has swapped in between.
//   4. Retries from step 1 on failure, freeing the failed allocation.
//
// This is the foundation of all lock-free "copy-on-write" updates and is
// used in the kernel for lock-free waitqueue manipulation, sk_buff pointer
// swaps, and many other places.
//
// Key API (kernel-memory-ordering-apis.md §2.4):
//   cmpxchg(ptr, old, new) — full barrier CAS returning old value
//
// Folly analogue: folly::atomic_shared_ptr update loop.
//
// Ordering edges of interest for pointer analysis:
//   update: kmalloc() → new_obj       HeapObjVar (candidate)
//           cmpxchg(&g_current, ...) → g_current now →{new heap} on success
//   read:   READ_ONCE(g_current) → p   p →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-11: lock-free pointer update via cmpxchg loop");

struct state {
	int version;
	int data;
};

static struct state *g_current;

/* Atomically replace g_current with a new State(ver, dat).
 * Returns 0 on success.  Frees the old state. */
static int cas_update(int ver, int dat)
{
	struct state *old_state, *new_state, *prev;

	new_state = kmalloc(sizeof(*new_state), GFP_KERNEL);  /* HeapObjVar */
	if (!new_state)
		return -ENOMEM;

	new_state->version = ver;
	new_state->data    = dat;

	/*
	 * CAS loop: retry until we atomically swap in new_state.
	 * On each failed attempt we update our view of old_state.
	 */
	do {
		old_state = READ_ONCE(g_current);             /* old_state →{heap|NULL} */
	} while ((prev = cmpxchg(&g_current, old_state, new_state)) != old_state);
	/* g_current →{new heap} */

	kfree(old_state);   /* safe: we own it exclusively after winning the CAS */
	return 0;
}

static int __init cas_init(void)
{
	/* Bootstrap: install initial state. */
	g_current = kmalloc(sizeof(*g_current), GFP_KERNEL); /* HeapObjVar */
	if (!g_current)
		return -ENOMEM;
	g_current->version = 0;
	g_current->data    = 0;

	/* Simulate a concurrent update. */
	cas_update(1, 42);
	cas_update(2, 99);

	return 0;
}

static void __exit cas_exit(void)
{
	struct state *p = READ_ONCE(g_current);   /* p →{heap} */

	if (p) {
		pr_info("cas: version=%d data=%d\n", p->version, p->data);
		kfree(p);
	}
}

module_init(cas_init);
module_exit(cas_exit);
