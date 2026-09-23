// SPDX-License-Identifier: GPL-2.0
// 01_mp_release_acquire.c — Message-Passing via smp_store_release / smp_load_acquire
//
// Non-blocking pattern: RELEASE-ACQUIRE message passing.
//
// A producer thread allocates a payload on the heap, initialises it, then
// publishes the pointer atomically with a *release* store.  A consumer thread
// acquires the pointer with an *acquire* load; the acquire ordering guarantees
// it observes all writes the producer performed before the release store.
//
// This is the canonical "store-release / load-acquire" non-blocking pattern:
// no lock is ever taken, yet the consumer is guaranteed to see a fully
// initialised payload once it reads a non-NULL pointer.
//
// Folly analogue: folly::AtomicStruct<T*> with memory_order_release /
//                 memory_order_acquire.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → msg              HeapObjVar
//          msg->value = 42              field write on heap obj
//          smp_store_release(&g_ptr, msg)  g_ptr →{heap}  (release store)
//   exit:  smp_load_acquire(&g_ptr) → msg  load of g_ptr, result →{heap}
//          msg->value                   field read through acquired ptr

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-01: message passing via smp_store_release/smp_load_acquire");

struct payload {
	int  value;
	char tag[8];
};

/* Global channel: producer writes, consumer reads. */
static struct payload *g_ptr;

static int __init mp01_init(void)
{
	struct payload *msg;

	/* [Producer side] -------------------------------------------------- */
	msg = kmalloc(sizeof(*msg), GFP_KERNEL);   /* g_ptr -> {heap} */
	if (!msg)
		return -ENOMEM;

	msg->value = 42;
	__builtin_memcpy(msg->tag, "hello\0\0\0", 8);

	/*
	 * Release store: all writes above (msg->value, msg->tag) are visible
	 * to any thread that subsequently acquires g_ptr with smp_load_acquire.
	 */
	smp_store_release(&g_ptr, msg);            /* PUBLISH: g_ptr →{heap} */
	return 0;
}

static void __exit mp01_exit(void)
{
	struct payload *msg;

	/* [Consumer side] -------------------------------------------------- */

	/*
	 * Acquire load: if non-NULL, we are guaranteed to observe every store
	 * the producer performed before the paired smp_store_release above.
	 */
	msg = smp_load_acquire(&g_ptr);            /* CONSUME: msg →{heap} */
	if (!msg)
		return;

	pr_info("mp01: value=%d tag=%.8s\n", msg->value, msg->tag);
	kfree(msg);
}

module_init(mp01_init);
module_exit(mp01_exit);
