// SPDX-License-Identifier: GPL-2.0
// 35_atomic_notify.c — Atomic notification (futex-like wait/wake on atomic)
//
// Non-blocking pattern: ATOMIC NOTIFICATION (folly::AtomicNotification).
//
// Threads wait on an atomic variable using the kernel's futex-like mechanism
// (or a busy-wait equivalent).  The notifier stores a value with a release
// store; waiters observe the change with an acquire load.  If the value has
// already changed, the waiter returns immediately (non-blocking fast path).
//
// This models the pattern behind:
//   - folly::AtomicNotification (atomic_notify_one/all in C++20)
//   - Linux kernel's wait_var_event / wake_up_var (5.1+)
//   - The kernel's prepare_to_wait + wake_up pattern on an atomic condition
//
// Protocol:
//   Notifier:  smp_store_release(&g_cond, NEW_VAL)  — update + release
//   Waiter:    while (smp_load_acquire(&g_cond) != EXPECTED) cpu_relax()
//              → then: g_result is safe to read
//
// Key APIs (kernel-memory-ordering-apis.md §2.2, §3.6):
//   smp_store_release   — release store to the condition variable
//   smp_load_acquire    — acquire load in the polling loop
//   atomic_set_release  — could be used equivalently on atomic_t
//
// Ordering edges of interest:
//   notify: kmalloc() → result; g_result = result; smp_store_release(g_cond)
//           g_result →{heap} ordered before g_cond store
//   wait:   smp_load_acquire(g_cond) → nonzero; g_result → p  p →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-35: atomic notification (futex-like wait/wake on a release store)");

#define COND_IDLE   0
#define COND_READY  1

struct notify_result {
	int  status;
	long data;
};

static int                  g_cond;   /* condition variable (plain int, not atomic_t) */
static struct notify_result *g_result;

/* Notifier: publish result then signal readiness via release store. */
static void do_notify(struct notify_result *result)
{
	/*
	 * Store result pointer first (plain store — single writer, sequenced
	 * before the release store on g_cond).
	 */
	g_result = result;                         /* g_result →{heap} */

	/*
	 * Release store: all stores above (g_result) are visible to any
	 * thread that subsequently observes g_cond != COND_IDLE via an
	 * acquire load.
	 */
	smp_store_release(&g_cond, COND_READY);    /* NOTIFY */
}

/* Waiter: spin until condition is set (acquire load). */
static struct notify_result *do_wait(void)
{
	/*
	 * Acquire loop: pairs with smp_store_release in do_notify.
	 * Once we exit the loop we are guaranteed to see g_result →{heap}.
	 */
	while (smp_load_acquire(&g_cond) != COND_READY)
		cpu_relax();

	return g_result;   /* g_result →{heap} (visible after acquire) */
}

static int __init notify_init(void)
{
	struct notify_result *r;

	r = kmalloc(sizeof(*r), GFP_KERNEL);       /* HeapObjVar */
	if (!r)
		return -ENOMEM;
	r->status = 0;
	r->data   = 0xabcdef;

	do_notify(r);                              /* g_cond = COND_READY */
	pr_info("atomic_notify: notification sent\n");
	return 0;
}

static void __exit notify_exit(void)
{
	struct notify_result *p = do_wait();       /* p →{heap} */

	if (p) {
		pr_info("atomic_notify: status=%d data=0x%lx\n",
			p->status, p->data);
		kfree(p);
	}
}

module_init(notify_init);
module_exit(notify_exit);
