// SPDX-License-Identifier: GPL-2.0
// 05_event_baton.c — One-shot event via atomic flag (Baton pattern)
//
// Non-blocking pattern: BATON (one-shot, non-blocking event).
//
// A "baton" (named after folly::Baton) is a one-shot rendezvous:
//   - The poster stores 1 with a release barrier, making all prior stores
//     visible to any thread that sees the flag as 1.
//   - The waiter spins (or may sleep) reading the flag with an acquire
//     barrier; once it sees 1 it is guaranteed to observe the payload.
//
// In the kernel this maps to:
//   Post:  atomic_set_release(&g_ready, 1)
//   Wait:  spin on atomic_read_acquire(&g_ready) until non-zero
//
// The pattern is *non-blocking*: the poster never blocks.  The waiter
// may spin (obstruction-free) or yield via cpu_relax().
//
// Folly: folly::Baton::post() / folly::Baton::wait()
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → payload           HeapObjVar
//          g_payload = payload            g_payload →{heap}
//          atomic_set_release(&g_ready,1) release fence on {heap} writes
//   exit:  atomic_read_acquire(&g_ready)  acquire fence
//          g_payload → msg               msg →{heap} (observed after acquire)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-05: baton one-shot event via atomic_set_release / atomic_read_acquire");

struct baton_payload {
	int data;
};

static atomic_t             g_ready   = ATOMIC_INIT(0);
static struct baton_payload *g_payload;

static int __init baton_init(void)
{
	struct baton_payload *p;

	/* [Poster side] ---------------------------------------------------- */
	p = kmalloc(sizeof(*p), GFP_KERNEL);       /* HeapObjVar */
	if (!p)
		return -ENOMEM;

	p->data    = 0xdeadbeef;
	g_payload  = p;                            /* g_payload →{heap} */

	/*
	 * Release store on the flag: all stores above (g_payload, p->data)
	 * happen-before any acquire load that observes g_ready == 1.
	 */
	atomic_set_release(&g_ready, 1);           /* POST */
	return 0;
}

static void __exit baton_exit(void)
{
	struct baton_payload *msg;

	/* [Waiter side] ---------------------------------------------------- */

	/*
	 * Spin until the flag is set.  The acquire semantics ensure we see
	 * all stores the poster did before atomic_set_release(&g_ready, 1).
	 */
	while (!atomic_read_acquire(&g_ready))     /* WAIT */
		cpu_relax();

	/*
	 * Safe to dereference g_payload: the acquire above pairs with the
	 * release store in baton_init, so g_payload →{heap} is visible here.
	 */
	msg = g_payload;                           /* msg →{heap} */
	if (msg) {
		pr_info("baton: data=0x%x\n", msg->data);
		kfree(msg);
	}
}

module_init(baton_init);
module_exit(baton_exit);
