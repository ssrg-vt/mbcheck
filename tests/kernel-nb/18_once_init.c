// SPDX-License-Identifier: GPL-2.0
// 18_once_init.c — Once-only initialization (DO_ONCE / call_once equivalent)
//
// Non-blocking pattern: ONCE INITIALIZATION via atomic CAS.
//
// Ensures an expensive initialisation runs exactly once across all CPUs,
// without a lock on the fast path.  State machine:
//   UNINIT (0) → RUNNING (1) → DONE (2)
//
// Fast path (lock-free, constant time):
//   atomic_read(&g_state) == DONE  →  return g_obj directly
//
// Slow path (first caller wins via CAS):
//   CAS(g_state, UNINIT, RUNNING) — exactly one thread transitions this
//   Winner: allocate + init, store to g_obj, then atomic_set(DONE)
//   Losers: spin-wait until state == DONE, then read g_obj
//
// This mirrors the kernel's DO_ONCE() macro and is equivalent to C++11
// std::call_once / folly::CallOnce.
//
// Key APIs (kernel-memory-ordering-apis.md §2.2, §2.4):
//   atomic_cmpxchg(&s, UNINIT, RUNNING)    — CAS for winner election
//   smp_store_release(&g_obj, obj)          — release store of result ptr
//   atomic_set_release(&g_state, DONE)      — signal completion
//   smp_load_acquire(&g_obj)               — acquire load on fast path
//
// Ordering edges of interest for pointer analysis:
//   winner: kmalloc() → obj            HeapObjVar
//           smp_store_release(&g_obj, obj) g_obj →{heap}
//   reader: smp_load_acquire(&g_obj) → p   p →{heap} (after DONE)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-18: once-only init via atomic CAS state machine");

#define STATE_UNINIT  0
#define STATE_RUNNING 1
#define STATE_DONE    2

struct singleton_data {
	int  magic;
	int  computed;
};

static atomic_t              g_state = ATOMIC_INIT(STATE_UNINIT);
static struct singleton_data *g_obj;

/* Returns the singleton, initialising it exactly once. */
static struct singleton_data *once_get(void)
{
	struct singleton_data *obj;

	/* Fast path: already done. */
	if (atomic_read(&g_state) == STATE_DONE) {
		/* Acquire: pairs with atomic_set_release(DONE) below. */
		return smp_load_acquire(&g_obj);           /* →{heap} */
	}

	/* Slow path: race to become the winner. */
	if (atomic_cmpxchg(&g_state, STATE_UNINIT, STATE_RUNNING) == STATE_UNINIT) {
		/* We are the winner. */
		obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
		if (!obj) {
			atomic_set(&g_state, STATE_UNINIT);
			return NULL;
		}
		obj->magic    = 0xc001;
		obj->computed = 42 * 1000;

		/*
		 * Release store: obj is fully initialised before g_obj is
		 * visible to any reader that sees STATE_DONE.
		 */
		smp_store_release(&g_obj, obj);            /* g_obj →{heap} */

		/* Signal completion: any reader that sees DONE sees g_obj. */
		atomic_set_release(&g_state, STATE_DONE);
		return obj;
	}

	/* Loser: spin until the winner finishes. */
	while (atomic_read(&g_state) != STATE_DONE)
		cpu_relax();

	return smp_load_acquire(&g_obj);               /* →{heap} */
}

static int __init once_init(void)
{
	struct singleton_data *s = once_get();    /* s →{heap} */

	if (!s)
		return -ENOMEM;
	pr_info("once: magic=0x%x computed=%d\n", s->magic, s->computed);
	return 0;
}

static void __exit once_exit(void)
{
	struct singleton_data *s = smp_load_acquire(&g_obj); /* s →{heap|NULL} */

	if (s)
		kfree(s);
}

module_init(once_init);
module_exit(once_exit);
