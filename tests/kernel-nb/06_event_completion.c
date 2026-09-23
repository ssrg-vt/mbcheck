// SPDX-License-Identifier: GPL-2.0
// 06_event_completion.c — One-shot event via kernel completion
//
// Non-blocking pattern: kernel COMPLETION as a one-shot event.
//
// struct completion wraps a wait_queue_head and an atomic "done" counter.
// complete() does a store-release on the counter then issues a wakeup.
// wait_for_completion() does a load-acquire on the counter; if it is
// already non-zero it returns immediately without blocking.
//
// From the pointer-analysis perspective the interesting flow is:
//   - Producer allocates heap data, stores it to g_result, then calls
//     complete(&g_done) — the release inside complete() orders the heap
//     write before the event signal.
//   - Consumer calls wait_for_completion(&g_done) — the acquire inside
//     pairs with the release, so after returning g_result →{heap}.
//
// Key APIs (kernel-memory-ordering-apis.md §3.5):
//   complete(c)              — store-release on completion counter
//   wait_for_completion(c)   — load-acquire; blocks or returns immediately
//
// Folly analogue: folly::Baton (blocking variant) or std::latch (C++20).
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → result        HeapObjVar
//          g_result = result          g_result →{heap}
//          complete(&g_done)          release: g_result write ordered before
//   exit:  wait_for_completion(...)   acquire: g_result →{heap} visible here
//          g_result → r               r →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-06: one-shot event via kernel completion");

struct result_obj {
	int status;
	int value;
};

static DECLARE_COMPLETION(g_done);
static struct result_obj *g_result;

static int __init compl_init(void)
{
	struct result_obj *result;

	/* [Signal side] ---------------------------------------------------- */
	result = kmalloc(sizeof(*result), GFP_KERNEL);  /* HeapObjVar */
	if (!result)
		return -ENOMEM;

	result->status = 0;       /* success */
	result->value  = 99;
	g_result = result;                              /* g_result →{heap} */

	/*
	 * complete() does a store-release on the internal done counter and
	 * wakes any waiters.  All stores above are ordered before the signal.
	 */
	complete(&g_done);                              /* SIGNAL */
	return 0;
}

static void __exit compl_exit(void)
{
	struct result_obj *r;

	/*
	 * wait_for_completion() does a load-acquire on the done counter.
	 * Since compl_init already called complete(), this returns immediately
	 * without sleeping.  The acquire pairs with the release in complete(),
	 * so g_result →{heap} is visible here.
	 */
	wait_for_completion(&g_done);                   /* WAIT (acquire) */

	r = g_result;                                   /* r →{heap} */
	if (r) {
		pr_info("compl: status=%d value=%d\n", r->status, r->value);
		kfree(r);
	}
}

module_init(compl_init);
module_exit(compl_exit);
